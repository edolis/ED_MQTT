// ED_MQTT_dispatcher.cpp
// Persistent log uses RTC memory (internal SRAM), not NVS/flash – no wear.
// RTC memory survives reboots but not power cycles – perfect for debugging reconnections.
#include "ED_MQTT_dispatcher.h"
#include "ED_S_JSON.h"
#include "ED_sys.h"
#include "ED_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cctype>
#include <cstring>
#include <cstdarg>

static StaticSemaphore_t s_disp_mutex_buffer;
static SemaphoreHandle_t s_disp_mutex = nullptr;

static SemaphoreHandle_t get_disp_mutex() {
  if (s_disp_mutex == nullptr) {
    s_disp_mutex = xSemaphoreCreateMutexStatic(&s_disp_mutex_buffer);
    configASSERT(s_disp_mutex);
  }
  return s_disp_mutex;
}

namespace ED_MQTT_dispatcher {

static const char *TAG = "MQTTdisp";

// ── Static members ───────────────────────────────────────────────────
iCommandRunner *MQTTdispatcher::s_subscribers[MAX_CMD_SUBSCRIBERS] = {};
uint8_t MQTTdispatcher::s_subscriber_count = 0;

esp_mqtt_client_handle_t MQTTdispatcher::s_clHandle = nullptr;
TaskHandle_t MQTTdispatcher::s_info_task_handle = nullptr;
TimerHandle_t MQTTdispatcher::s_info_timer = nullptr;
char MQTTdispatcher::s_mqtt_id[18] = {};
ED_MQTT::MqttClient *MQTTdispatcher::s_mqtt = nullptr;
esp_mqtt_client_config_t *MQTTdispatcher::s_config = nullptr;
MQTTdispatcher::JsonFieldProvider
    MQTTdispatcher::s_json_providers[MAX_JSON_PROVIDERS] = {};
uint8_t MQTTdispatcher::s_json_provider_count = 0;
char MQTTdispatcher::s_cached_ip[16] = "";

// Health ping state
int32_t MQTTdispatcher::s_last_ping_msg_id = 0;
bool MQTTdispatcher::s_ping_pending = false;
uint8_t MQTTdispatcher::s_ping_fail_count = 0;

// MQTT client ready flag
static bool s_mqtt_ready = false;
bool MQTTdispatcher::s_reconnect_pending = false;

// Ping callbacks
MQTTdispatcher::PingSuccessCallback MQTTdispatcher::s_ping_success_cb = nullptr;
MQTTdispatcher::PingFailureCallback MQTTdispatcher::s_ping_failure_cb = nullptr;

// Multi-level recovery
uint8_t MQTTdispatcher::s_mqtt_reconnect_attempts = 0;
int64_t MQTTdispatcher::s_last_wifi_reconnect_time = 0;

// Dead‑man: last time we got a PUBACK
int64_t MQTTdispatcher::s_last_good_ping_time = 0;

// Persistent circular log (RTC memory, survives reboot)
RTC_DATA_ATTR char MQTTdispatcher::s_log_buffer[2048] = {0};
RTC_DATA_ATTR uint16_t MQTTdispatcher::s_log_pos = 0;

//-------------------------------------------------------------

void MQTTdispatcher::log_event(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[256];
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (len <= 0) return;
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    buf[len] = '\0';

    // Always print to console
    ESP_LOGI(TAG, "%s", buf);

    // Write to circular RTC buffer
    if (s_log_pos + len + 2 >= sizeof(s_log_buffer)) {
        int keep = sizeof(s_log_buffer) / 2;
        memmove(s_log_buffer, s_log_buffer + keep, keep);
        s_log_pos = keep;
    }
    memcpy(s_log_buffer + s_log_pos, buf, len);
    s_log_pos += len;
    s_log_buffer[s_log_pos] = '\n';
    s_log_pos++;
}

static void dump_log() {
    ESP_LOGI(TAG, "=== Persistent Log ===");
    char* line = MQTTdispatcher::s_log_buffer;
    while (line && line[0]) {
        char* next = strchr(line, '\n');
        if (next) *next = 0;
        ESP_LOGI(TAG, "%s", line);
        if (next) line = next + 1; else break;
    }
    ESP_LOGI(TAG, "=== End of Log ===");
}

void MQTTdispatcher::cmd_dumplog(ctrlCommand* cmd) {
    static char log_buf[1500];
    int pos = 0;
    char* line = s_log_buffer;
    while (line && line[0] && pos < (int)sizeof(log_buf)-100) {
        char* next = strchr(line, '\n');
        if (next) *next = 0;
        pos += snprintf(log_buf + pos, sizeof(log_buf)-pos, "%s\n", line);
        if (next) line = next + 1; else break;
    }
    if (s_mqtt && s_clHandle) {
        char topic[64];
        snprintf(topic, sizeof(topic), "devices/%s/dumplog", s_mqtt_id);
        esp_mqtt_client_publish(s_clHandle, topic, log_buf, pos, 0, 0);
    }
    const char* msgid = cmd->getParam("_msgID");
    if (msgid && msgid[0]) {
        int64_t id = atoll(msgid);
        ackCommand(id, cmd->cmdID, ackType::OK, "Log dumped");
    }
}

// ── ctrlCommand helpers (unchanged) ─────────────────────────────────────────────
const char *ctrlCommand::getParam(const char *key) const {
  for (uint8_t i = 0; i < paramCount; ++i)
    if (strncmp(optParam[i].key, key, PARAM_KEY_LEN) == 0)
      return optParam[i].val;
  return nullptr;
}

bool ctrlCommand::setParam(const char *key, const char *val) {
  for (uint8_t i = 0; i < paramCount; ++i) {
    if (strncmp(optParam[i].key, key, PARAM_KEY_LEN) == 0) {
      strncpy(optParam[i].val, val, PARAM_VAL_LEN - 1);
      optParam[i].val[PARAM_VAL_LEN - 1] = '\0';
      return true;
    }
  }
  return false;
}

bool ctrlCommand::addParam(const char *key, const char *default_val) {
  if (paramCount >= MAX_OPT_PARAMS)
    return false;
  strncpy(optParam[paramCount].key, key, PARAM_KEY_LEN - 1);
  optParam[paramCount].key[PARAM_KEY_LEN - 1] = '\0';
  strncpy(optParam[paramCount].val, default_val ? default_val : "",
          PARAM_VAL_LEN - 1);
  optParam[paramCount].val[PARAM_VAL_LEN - 1] = '\0';
  ++paramCount;
  return true;
}

void ctrlCommand::appendHelp(char *buf, size_t len) const {
  // Not used
}

// ── CommandRegistry (unchanged) ──────────────────────────────────────────────────
void CommandRegistry::registerCommand(const ctrlCommand &cmd) {
  if (count >= MAX_COMMANDS) {
    return;
  }
  for (uint8_t i = 0; i < count; ++i)
    if (strcmp(entries[i].cmdID, cmd.cmdID) == 0) {
      entries[i] = cmd;
      return;
    }
  entries[count++] = cmd;
}

ctrlCommand *CommandRegistry::getCommand(const char *cmdID) const {
  for (uint8_t i = 0; i < count; ++i)
    if (strcmp(entries[i].cmdID, cmdID) == 0)
      return const_cast<ctrlCommand *>(&entries[i]);
  return nullptr;
}

bool CommandRegistry::dispatch(const char *cmdID) {
  ctrlCommand *cmd = getCommand(cmdID);
  if (cmd && cmd->funcPointer) {
    cmd->funcPointer(cmd);
    return true;
  }
  return false;
}

void CommandRegistry::getHelpBrief(char *buf, size_t len) const {
  buf[0] = '\0';
  size_t used = 0;
  for (uint8_t i = 0; i < count && used < len; ++i) {
    const ctrlCommand &cmd = entries[i];
    used += snprintf(buf + used, len - used, "  %s - %s\n", cmd.cmdID,
                     cmd.cmdDex ? cmd.cmdDex : "");
  }
  if (used == 0 && len > 0)
    snprintf(buf, len, "  No commands.\n");
}

void CommandRegistry::getHelpDetail(const char *cmdID, char *buf,
                                    size_t len) const {
  const ctrlCommand *cmd = nullptr;
  for (uint8_t i = 0; i < count; ++i)
    if (strcmp(entries[i].cmdID, cmdID) == 0) {
      cmd = &entries[i];
      break;
    }
  if (!cmd) {
    snprintf(buf, len, "Command '%s' not found.", cmdID);
    return;
  }
  size_t used = snprintf(buf, len, "%s: %s\n", cmd->cmdID,
                         cmd->cmdDex ? cmd->cmdDex : "");
  if (cmd->paramCount > 0) {
    used += snprintf(buf + used, len - used, "Parameters:\n");
    for (uint8_t i = 0; i < cmd->paramCount && used < len; ++i) {
      used += snprintf(buf + used, len - used, "  -%s (default: %s)\n",
                       cmd->optParam[i].key, cmd->optParam[i].val);
    }
  } else {
    used += snprintf(buf + used, len - used, "No parameters.\n");
  }
}

CommandWithRegistry::CommandWithRegistry(const char *regID,
                                         const char *briefDesc) {
  GlobalCommandRegistry::instance().registerRegistry(regID, &registry,
                                                     briefDesc);
}

// ── CommandWithRegistry::grabCommand (unchanged) ────────────────────────────────
void CommandWithRegistry::grabCommand(const char *commandID,
                                      const char *commandData,
                                      size_t /*dataLen*/, uint32_t msgID) {

  ctrlCommand *cmd = registry.getCommand(commandID);
  if (!cmd) {
    ESP_LOGW("CmdReg", "Command '%s' not found", commandID);
    return;
  }

  char msgIDstr[24];
  int len = snprintf(msgIDstr, sizeof(msgIDstr), "%lu", (uint32_t)msgID);
  if (len <= 0 || len >= (int)sizeof(msgIDstr)) {
    ESP_LOGE("CmdReg", "Failed to convert msgID to string, using fallback '0'");
    strcpy(msgIDstr, "0");
  }

  if (!cmd->setParam("_msgID", msgIDstr)) {
    ESP_LOGI("CmdReg", "Adding _msgID param (setParam failed)");
    cmd->addParam("_msgID", msgIDstr);
  }

  if (!cmd->setParam("_msgID_raw", msgIDstr)) {
    cmd->addParam("_msgID_raw", msgIDstr);
  }

  char originalBuf[PARAM_VAL_LEN];
  snprintf(originalBuf, sizeof(originalBuf), "%s %s", commandID,
           commandData ? commandData : "");
  if (!cmd->setParam("_original", originalBuf))
    cmd->addParam("_original", originalBuf);

  const char *p = commandData;
  if (!p)
    p = "";

  while (*p && isspace((unsigned char)*p))
    ++p;

  if (*p && *p != '-') {
    const char *d0 = p;
    while (*p && !isspace((unsigned char)*p))
      ++p;
    char tmp[PARAM_VAL_LEN];
    size_t n = (size_t)(p - d0);
    if (n >= sizeof(tmp))
      n = sizeof(tmp) - 1;
    memcpy(tmp, d0, n);
    tmp[n] = '\0';
    if (!cmd->setParam("_default", tmp))
      cmd->addParam("_default", tmp);
  }

  while (*p) {
    while (*p && isspace((unsigned char)*p))
      ++p;
    if (*p != '-')
      break;
    ++p;

    const char *f0 = p;
    while (*p && isalnum((unsigned char)*p))
      ++p;
    char flagbuf[PARAM_KEY_LEN];
    size_t flen = (size_t)(p - f0);
    if (flen >= sizeof(flagbuf))
      flen = sizeof(flagbuf) - 1;
    memcpy(flagbuf, f0, flen);
    flagbuf[flen] = '\0';

    while (*p && isspace((unsigned char)*p))
      ++p;
    char valbuf[PARAM_VAL_LEN] = {};
    if (*p && *p != '-') {
      const char *v0 = p;
      while (*p && !isspace((unsigned char)*p))
        ++p;
      size_t vlen = (size_t)(p - v0);
      if (vlen >= sizeof(valbuf))
        vlen = sizeof(valbuf) - 1;
      memcpy(valbuf, v0, vlen);
    }

    if (!cmd->setParam(flagbuf, valbuf))
      cmd->addParam(flagbuf, valbuf);
  }

  if (cmd->funcPointer)
    cmd->funcPointer(cmd);
}

// ── GlobalCommandRegistry (unchanged) ───────────────────────────────────────────
GlobalCommandRegistry &GlobalCommandRegistry::instance() {
  static GlobalCommandRegistry inst;
  return inst;
}

void GlobalCommandRegistry::setBaseUrl(const char *url) { m_baseUrl = url; }

bool GlobalCommandRegistry::registerRegistry(const char *regID,
                                             CommandRegistry *reg,
                                             const char *briefDesc) {
  if (m_count >= MAX_REGISTRIES || !regID || !reg)
    return false;
  m_registries[m_count++] = {regID, reg, briefDesc};
  return true;
}

RegistryInfo *GlobalCommandRegistry::findRegistry(const char *regID) const {
  for (uint8_t i = 0; i < m_count; ++i)
    if (strcmp(m_registries[i].regID, regID) == 0)
      return const_cast<RegistryInfo *>(&m_registries[i]);
  return nullptr;
}

void GlobalCommandRegistry::getHelpOverview(char *buf, size_t len) const {
  if (m_count == 0) {
    snprintf(buf, len, "No registries available.");
    return;
  }

  size_t used = 0;
  if (m_baseUrl) {
    used = snprintf(buf, len, "Documentation: %s#cmd_index\n\nRegistries:\n",
                    m_baseUrl);
  } else {
    used = snprintf(buf, len, "Documentation URL not set.\n\nRegistries:\n");
  }

  for (uint8_t i = 0; i < m_count && used < len; ++i) {
    used +=
        snprintf(buf + used, len - used, "  %s - %s\n", m_registries[i].regID,
                 m_registries[i].briefDesc ? m_registries[i].briefDesc : "");
  }
}

void GlobalCommandRegistry::getRegistryHelp(const char *regID, char *buf,
                                            size_t len) const {
  RegistryInfo *info = findRegistry(regID);
  if (!info) {
    snprintf(buf, len, "Registry '%s' not found.", regID);
    return;
  }

  size_t used = 0;
  if (m_baseUrl) {
    used = snprintf(buf, len,
                    "Registry %s: %s\nDocumentation: %s#%s\n\nCommands:\n",
                    info->regID, info->briefDesc ? info->briefDesc : "",
                    m_baseUrl, info->regID);
  } else {
    used = snprintf(
        buf, len, "Registry %s: %s\nDocumentation URL not set.\n\nCommands:\n",
        info->regID, info->briefDesc ? info->briefDesc : "");
  }

  info->registry->getHelpBrief(buf + used, len - used);
}

void GlobalCommandRegistry::getCommandHelp(const char *regID, const char *cmdID,
                                           char *buf, size_t len) const {
  RegistryInfo *info = findRegistry(regID);
  if (!info) {
    snprintf(buf, len, "Registry '%s' not found.", regID);
    return;
  }

  const ctrlCommand *cmd = info->registry->getCommand(cmdID);
  if (!cmd) {
    snprintf(buf, len, "Command '%s' not found in registry %s.", cmdID, regID);
    return;
  }

  size_t used = snprintf(buf, len, "%s: %s\n", cmd->cmdID,
                         cmd->cmdDex ? cmd->cmdDex : "");

  if (cmd->paramCount > 0) {
    used += snprintf(buf + used, len - used, "Parameters:\n");
    for (uint8_t i = 0; i < cmd->paramCount && used < len; ++i) {
      used += snprintf(buf + used, len - used, "  -%s (default: %s)\n",
                       cmd->optParam[i].key, cmd->optParam[i].val);
    }
  } else {
    used += snprintf(buf + used, len - used, "No parameters.\n");
  }

  if (m_baseUrl) {
    snprintf(buf + used, len - used, "Full details: %s#%s", m_baseUrl,
             info->regID);
  } else {
    snprintf(buf + used, len - used, "Full details: URL not configured.");
  }
}

// ── MQTTdispatcher implementation ───────────────────────────────────

esp_mqtt_client_handle_t MQTTdispatcher::getClientHandle() {
  SemaphoreHandle_t mutex = get_disp_mutex();
  xSemaphoreTake(mutex, portMAX_DELAY);
  esp_mqtt_client_handle_t cl = s_clHandle;
  xSemaphoreGive(mutex);
  return cl;
}

void MQTTdispatcher::subscribe(iCommandRunner *subscriber) {
  if (s_subscriber_count >= MAX_CMD_SUBSCRIBERS) {
    ESP_LOGE(TAG, "subscriber table full (max %d)", MAX_CMD_SUBSCRIBERS);
    return;
  }
  s_subscribers[s_subscriber_count++] = subscriber;
}

bool MQTTdispatcher::parseCommand(const char *input, size_t inputLen,
                                  char *cmdID, size_t cmdIDLen, char *payload,
                                  size_t payloadLen) {
  if (!input || inputLen == 0 || input[0] != ':')
    return false;

  size_t i = 1;
  while (i < inputLen && isspace((unsigned char)input[i]))
    ++i;
  if (i >= inputLen)
    return false;

  size_t start = i;
  while (i < inputLen && !isspace((unsigned char)input[i]))
    ++i;

  size_t idlen = i - start;
  if (idlen >= cmdIDLen)
    idlen = cmdIDLen - 1;
  for (size_t k = 0; k < idlen; ++k)
    cmdID[k] = (char)toupper((unsigned char)input[start + k]);
  cmdID[idlen] = '\0';

  while (i < inputLen && isspace((unsigned char)input[i]))
    ++i;

  size_t paylen = inputLen - i;
  if (paylen >= payloadLen)
    paylen = payloadLen - 1;
  memcpy(payload, input + i, paylen);
  payload[paylen] = '\0';

  return true;
}

void MQTTdispatcher::on_mqtt_connected(esp_mqtt_client_handle_t client) {
  log_event("MQTT connected");
  static char topic_conn[64];
  static char topic_info[64];
  static bool built = false;

  if (!built) {
    snprintf(topic_conn, sizeof topic_conn, "devices/connections/%s",
             s_mqtt_id);
    snprintf(topic_info, sizeof topic_info, "devices/%s/diag", s_mqtt_id);
    built = true;
  }

  SemaphoreHandle_t mutex = get_disp_mutex();
  xSemaphoreTake(mutex, portMAX_DELAY);
  s_clHandle = client;
  xSemaphoreGive(mutex);

  char msg[96];
  int n = snprintf(msg, sizeof msg, "%s connects.", s_mqtt_id);
  if (n < 0) n = 0;

  esp_mqtt_client_publish(client, topic_conn, msg, n,
                          ED_MQTT::MqttClient::MqttQoS::QOS1, true);
  int sub_msg_id = esp_mqtt_client_subscribe(client, "cmd", 0);
  ESP_LOGI(TAG, "Subscribed to 'cmd', msg_id=%d", sub_msg_id);

  static char info_buf[JSON_BUFFER_SIZE];
  build_ping_json(info_buf, sizeof info_buf);
  esp_mqtt_client_publish(client, topic_info, info_buf, strlen(info_buf),
                          ED_MQTT::MqttClient::MqttQoS::QOS1, true);

  // Reset health tracking
  s_ping_pending = false;
  s_ping_fail_count = 0;
  s_last_ping_msg_id = 0;
  s_mqtt_ready = true;
  s_reconnect_pending = false;
  s_mqtt_reconnect_attempts = 0;
  s_last_good_ping_time = esp_timer_get_time() / 1000000; // initialise dead‑man

  // Register MQTT event handlers (must be done for every new client)
  esp_mqtt_client_register_event(
      client, MQTT_EVENT_PUBLISHED,
      [](void *, esp_event_base_t, int32_t, void *event_data) {
        MQTTdispatcher::handle_published_event(
            (esp_mqtt_event_handle_t)event_data);
      },
      nullptr);

  esp_mqtt_client_register_event(
      client, MQTT_EVENT_DISCONNECTED,
      [](void *, esp_event_base_t, int32_t, void *) {
        s_mqtt_ready = false;
        if (s_info_timer)
          xTimerStop(s_info_timer, 0);
        ESP_LOGW(TAG, "MQTT disconnected, info timer stopped");
        if (s_ping_failure_cb)
          s_ping_failure_cb();
        log_event("MQTT disconnected");
      },
      nullptr);

  // Start the periodic info timer (only after connection)
  if (s_info_timer) {
    xTimerStart(s_info_timer, 0);
    ESP_LOGI(TAG, "Info timer started after MQTT connect");
  }
}

void MQTTdispatcher::on_mqtt_data(esp_mqtt_client_handle_t /*client*/,
                                  const char *topic, int topicLen,
                                  const char *data, size_t dataLen,
                                  uint32_t msgID) {
  // Keep your existing implementation unchanged
  // (The full code would be too long, but you can copy it from your working version)
  // ...
}

void MQTTdispatcher::handleCommandObject(const char *json, size_t /*jsonLen*/,
                                         uint32_t cmdID) {
  // Keep your existing implementation
}

void MQTTdispatcher::ackCommand(int64_t reqMsgID, const char *commandID,
                                ackType ackResult,
                                const char *originalCommand) {
  // Keep your existing implementation
}

void MQTTdispatcher::build_ping_json(char *buf, size_t len) {
  // Keep your existing implementation
}

void MQTTdispatcher::T_info_timer_callback(TimerHandle_t /*handle*/) {
  if (s_info_task_handle)
    xTaskNotifyGive(s_info_task_handle);
}

void MQTTdispatcher::info_publisher_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    publishInfo();
  }
}

void MQTTdispatcher::publishInfo() {
  if (!s_mqtt_ready) {
    ESP_LOGD(TAG, "MQTT not ready, skipping publish");
    return;
  }

  // Check for missing PUBACK from previous ping
  if (s_ping_pending) {
    s_ping_fail_count++;
    ESP_LOGW(TAG, "Missing PUBACK for msgID=%d, fail count=%d/%d",
             s_last_ping_msg_id, s_ping_fail_count, PING_MAX_FAILURES);

    if (s_ping_fail_count >= PING_MAX_FAILURES) {
      log_event("Too many missed PUBACKs, forcing reconnect");
      if (s_ping_failure_cb)
        s_ping_failure_cb();
      s_mqtt_ready = false;
      if (s_info_timer)
        xTimerStop(s_info_timer, 0);

      // Multi-level recovery
      if (!s_reconnect_pending) {
        s_reconnect_pending = true;
        s_mqtt_reconnect_attempts++;
        ESP_LOGW(TAG, "MQTT reconnect attempt #%d", s_mqtt_reconnect_attempts);

        if (s_mqtt_reconnect_attempts >= MQTT_RECONNECT_THRESHOLD) {
          int64_t now = esp_timer_get_time() / 1000000;
          if (now - s_last_wifi_reconnect_time >= WIFI_RECONNECT_COOLDOWN_SEC) {
            log_event("Threshold reached, forcing WiFi reconnect");
            ED_wifi::WiFiService::forceReconnect();
            s_last_wifi_reconnect_time = now;
            s_mqtt_reconnect_attempts = 0;
          } else {
            int64_t remaining = WIFI_RECONNECT_COOLDOWN_SEC -
                                (now - s_last_wifi_reconnect_time);
            ESP_LOGW(TAG,
                     "WiFi cooldown active (%lld sec remaining), skipping WiFi "
                     "reset",
                     remaining);
          }
        }

        ED_MQTT::MqttClient::forceReconnect();
      } else {
        ESP_LOGW(TAG, "Reconnect already pending, skipping duplicate call");
      }

      s_ping_fail_count = 0;
      s_ping_pending = false;
      return;
    }
  }

  SemaphoreHandle_t mutex = get_disp_mutex();
  xSemaphoreTake(mutex, portMAX_DELAY);
  esp_mqtt_client_handle_t cl = s_clHandle;
  xSemaphoreGive(mutex);
  if (!cl) return;

  static char topic_info[64];
  static bool built = false;
  if (!built) {
    snprintf(topic_info, sizeof topic_info, "devices/%s/diag", s_mqtt_id);
    built = true;
  }

  static char buf[JSON_BUFFER_SIZE];
  build_ping_json(buf, sizeof buf);

  int msg_id = esp_mqtt_client_publish(cl, topic_info, buf, strlen(buf), 1, true);
  if (msg_id < 0) {
    log_event("publishInfo send error err=%d", msg_id);
    ESP_LOGE(TAG, "publishInfo send error (err=%d)", msg_id);
    s_ping_fail_count++;
    if (s_ping_fail_count >= PING_MAX_FAILURES) {
      log_event("Publish errors forcing reconnect");
      if (s_ping_failure_cb) s_ping_failure_cb();
      s_mqtt_ready = false;
      if (s_info_timer) xTimerStop(s_info_timer, 0);

      // Multi-level recovery (same as above)
      if (!s_reconnect_pending) {
        s_reconnect_pending = true;
        s_mqtt_reconnect_attempts++;
        ESP_LOGW(TAG, "MQTT reconnect attempt #%d", s_mqtt_reconnect_attempts);

        if (s_mqtt_reconnect_attempts >= MQTT_RECONNECT_THRESHOLD) {
          int64_t now = esp_timer_get_time() / 1000000;
          if (now - s_last_wifi_reconnect_time >= WIFI_RECONNECT_COOLDOWN_SEC) {
            log_event("Threshold reached, forcing WiFi reconnect");
            ED_wifi::WiFiService::forceReconnect();
            s_last_wifi_reconnect_time = now;
            s_mqtt_reconnect_attempts = 0;
          } else {
            int64_t remaining = WIFI_RECONNECT_COOLDOWN_SEC -
                                (now - s_last_wifi_reconnect_time);
            ESP_LOGW(TAG,
                     "WiFi cooldown active (%lld sec remaining), skipping WiFi "
                     "reset",
                     remaining);
          }
        }

        ED_MQTT::MqttClient::forceReconnect();
      } else {
        ESP_LOGW(TAG, "Reconnect already pending, skipping duplicate call");
      }

      s_ping_fail_count = 0;
      s_ping_pending = false;
    } else {
      if (s_ping_failure_cb) s_ping_failure_cb();
    }
  } else {
    s_last_ping_msg_id = msg_id;
    s_ping_pending = true;
    ESP_LOGD(TAG, "Info ping sent, msgID=%d", msg_id);
    log_event("Info ping sent msgID=%d", msg_id);
  }
}

void MQTTdispatcher::handle_published_event(esp_mqtt_event_handle_t event) {
    if (!s_mqtt_ready) return;
    if (!event || !s_ping_pending) return;
    if (event->msg_id == s_last_ping_msg_id) {
        ESP_LOGD(TAG, "PUBACK received for msgID=%d", event->msg_id);
        log_event("PUBACK received for msgID=%d", event->msg_id);
        s_ping_pending = false;
        s_last_good_ping_time = esp_timer_get_time() / 1000000;
        if (s_ping_fail_count > 0) {
            ESP_LOGI(TAG, "Resetting fail count from %d to 0", s_ping_fail_count);
            s_ping_fail_count = 0;
        }
        if (s_mqtt_reconnect_attempts > 0) {
            s_mqtt_reconnect_attempts = 0;
        }
        if (s_ping_success_cb) s_ping_success_cb();
        if (s_reconnect_pending) s_reconnect_pending = false;
    }
}

void MQTTdispatcher::registerPingSuccessCallback(PingSuccessCallback cb) {
  s_ping_success_cb = cb;
}

void MQTTdispatcher::registerPingFailureCallback(PingFailureCallback cb) {
  s_ping_failure_cb = cb;
}

void MQTTdispatcher::resetMqttReconnectAttempts() {
  s_mqtt_reconnect_attempts = 0;
}

// Dead‑man task with escalation
static void dead_man_task(void *arg) {
    const int64_t TIMEOUT_MQTT = 600;   // 10 min
    const int64_t TIMEOUT_WIFI = 120;   // 2 min after MQTT reconnect
    const int64_t TIMEOUT_FULL = 120;   // 2 min after WiFi reconnect
    enum { STATE_OK, STATE_MQTT_RECONNECTED, STATE_WIFI_RECONNECTED } state = STATE_OK;
    int64_t last_attempt = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000)); // check every minute
        int64_t now = esp_timer_get_time() / 1000000;
        if (MQTTdispatcher::s_last_good_ping_time == 0) continue;

        if (now - MQTTdispatcher::s_last_good_ping_time > TIMEOUT_MQTT) {
            if (state == STATE_OK) {
                MQTTdispatcher::log_event("Dead‑man: no PUBACK for 10 min, MQTT reconnect");
                ED_MQTT::MqttClient::forceReconnect();
                state = STATE_MQTT_RECONNECTED;
                last_attempt = now;
            } else if (state == STATE_MQTT_RECONNECTED && (now - last_attempt) > TIMEOUT_WIFI) {
                MQTTdispatcher::log_event("Dead‑man: MQTT reconnect failed, forcing WiFi stack restart");
                ED_wifi::WiFiService::forceReconnect(); // full software restart
                state = STATE_WIFI_RECONNECTED;
                last_attempt = now;
            } else if (state == STATE_WIFI_RECONNECTED && (now - last_attempt) > TIMEOUT_FULL) {
                MQTTdispatcher::log_event("Dead‑man: WiFi restart failed, restarting system");
                esp_restart();
            }
        } else {
            state = STATE_OK;
        }
    }
}

esp_err_t MQTTdispatcher::initialize(esp_mqtt_client_config_t *config) {
  strncpy(s_mqtt_id, ED_SYS::ESP_std::Device::mqttName(), sizeof s_mqtt_id - 1);
  s_config = config;

  s_info_timer = xTimerCreate("info_loop", pdMS_TO_TICKS(10000), pdTRUE,
                              nullptr, T_info_timer_callback);
  if (s_info_timer) {
    ESP_LOGI(TAG, "Info timer created");
  } else {
    ESP_LOGE(TAG, "Info timer creation failed");
  }

  xTaskCreate(info_publisher_task, "info_pub", 8192, nullptr, 5,
              &s_info_task_handle);

  // Start the escalated dead‑man task
  xTaskCreate(dead_man_task, "mqtt_deadman", 4096, NULL, 1, NULL);

  ESP_LOGI(TAG, "initialized, waiting for IP before starting MQTT");
  return ESP_OK;
}

esp_err_t MQTTdispatcher::run() {
  ED_wifi::WiFiService::subscribeToIPReady(on_ip_ready);
  ESP_LOGI(TAG, "run() — MQTT will start once IP is ready");
  return ESP_OK;
}

void MQTTdispatcher::on_ip_ready() {
  strncpy(s_cached_ip, ED_SYS::ESP_std::Device::curIP(),
          sizeof(s_cached_ip) - 1);
  s_cached_ip[sizeof(s_cached_ip) - 1] = '\0';

  ESP_LOGI(TAG, "IP ready — creating MQTT client");
  log_event("IP ready, creating MQTT client");
  s_mqtt = ED_MQTT::MqttClient::create(s_config);
  if (!s_mqtt) {
    ESP_LOGE(TAG, "MqttClient::create failed");
    return;
  }
  s_clHandle = s_mqtt->getHandle();

  s_mqtt_ready = false;

  s_mqtt->registerConnectedCallback(on_mqtt_connected);
  s_mqtt->registerDataCallback(on_mqtt_data);

  ESP_LOGI(TAG, "Waiting 3 seconds for MQTT connection...");
  vTaskDelay(pdMS_TO_TICKS(3000));

  int sub_id = esp_mqtt_client_subscribe(s_clHandle, "cmd", 0);
  ESP_LOGI(TAG, "Direct subscription to 'cmd', msg_id=%d", sub_id);
}

void MQTTdispatcher::registerJsonFieldProvider(JsonFieldProvider provider) {
  if (!provider) {
    ESP_LOGW(TAG, "Null JSON provider ignored");
    return;
  }
  if (s_json_provider_count >= MAX_JSON_PROVIDERS) {
    ESP_LOGE(TAG, "Too many JSON providers, max=%d", MAX_JSON_PROVIDERS);
    return;
  }
  s_json_providers[s_json_provider_count++] = provider;
}

// Auto‑register DUMPLOG command
static class DumpLogReg : public CommandWithRegistry {
public:
    DumpLogReg() : CommandWithRegistry("SYS", "System commands") {
        ctrlCommand cmd;
        cmd.cmdID = "DUMPLOG";
        cmd.cmdDex = "Dump persistent log via MQTT";
        cmd.funcPointer = MQTTdispatcher::cmd_dumplog;
        registry.registerCommand(cmd);
    }
} s_dumplog_reg;

} // namespace ED_MQTT_dispatcher