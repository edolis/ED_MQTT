// ED_MQTT_dispatcher.cpp – Final Clean Version
#include "ED_MQTT_dispatcher.h"
#include "ED_S_JSON.h"
#include "ED_sys.h"
#include "ED_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include <cctype>
#include <cstring>

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

// ── Static members (class)
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

int32_t MQTTdispatcher::s_last_ping_msg_id = 0;
bool MQTTdispatcher::s_ping_pending = false;
uint8_t MQTTdispatcher::s_ping_fail_count = 0;

static bool s_mqtt_ready = false;
bool MQTTdispatcher::s_reconnect_pending = false;

MQTTdispatcher::PingSuccessCallback MQTTdispatcher::s_ping_success_cb = nullptr;
MQTTdispatcher::PingFailureCallback MQTTdispatcher::s_ping_failure_cb = nullptr;

uint8_t MQTTdispatcher::s_mqtt_reconnect_attempts = 0;
int64_t MQTTdispatcher::s_last_wifi_reconnect_time = 0;

int64_t MQTTdispatcher::s_last_good_ping_time = 0;

// ── Persistent log (RTC_NOINIT, survives soft reboot)
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
static __attribute__((section(".rtc_noinit"))) uint8_t s_log_buffer[2046];
static __attribute__((section(".rtc_noinit"))) uint16_t s_log_head;
static __attribute__((section(".rtc_noinit"))) uint16_t s_log_count;
static __attribute__((section(".rtc_noinit"))) uint32_t s_log_base_time;
static __attribute__((section(".rtc_noinit"))) uint16_t s_log_start_idx;
static __attribute__((section(".rtc_noinit"))) uint32_t s_log_magic;
static const uint32_t LOG_MAGIC = 0xA5A5A5A5;

static const char* event_code_to_string(uint8_t code) {
    switch (static_cast<MQTTdispatcher::EventCode>(code)) {
        case MQTTdispatcher::EventCode::IP_READY: return "IP ready";
        case MQTTdispatcher::EventCode::MQTT_CONNECTED: return "MQTT connected";
        case MQTTdispatcher::EventCode::MQTT_DISCONNECTED: return "MQTT disconnected";
        case MQTTdispatcher::EventCode::INFO_PING_SENT: return "Ping sent";
        case MQTTdispatcher::EventCode::PUBACK_RECEIVED: return "PUBACK";
        case MQTTdispatcher::EventCode::MISSING_PUBACK: return "Missing PUBACK";
        case MQTTdispatcher::EventCode::FORCE_RECONNECT: return "Force reconnect";
        case MQTTdispatcher::EventCode::WIFI_RECONNECT: return "WiFi reset";
        case MQTTdispatcher::EventCode::DEAD_MAN_MQTT: return "Dead‑man MQTT";
        case MQTTdispatcher::EventCode::DEAD_MAN_WIFI: return "Dead‑man WiFi";
        case MQTTdispatcher::EventCode::DEAD_MAN_RESTART: return "Dead‑man restart";
        case MQTTdispatcher::EventCode::PUBLISH_ERROR: return "Publish error";
        case MQTTdispatcher::EventCode::THRESHOLD_REACHED: return "Threshold WiFi";
        case MQTTdispatcher::EventCode::TOO_MANY_MISSED: return "Too many missed";
        case MQTTdispatcher::EventCode::INFO_PING_SKIPPED: return "Ping skipped";
        case MQTTdispatcher::EventCode::RECONNECT_ATTEMPT: return "Reconnect attempt";
        case MQTTdispatcher::EventCode::SOFT_REBOOT: return "Soft reboot";
        default: return "Unknown";
    }
}
#endif

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
  if (paramCount >= MAX_OPT_PARAMS) return false;
  strncpy(optParam[paramCount].key, key, PARAM_KEY_LEN - 1);
  optParam[paramCount].key[PARAM_KEY_LEN - 1] = '\0';
  strncpy(optParam[paramCount].val, default_val ? default_val : "", PARAM_VAL_LEN - 1);
  optParam[paramCount].val[PARAM_VAL_LEN - 1] = '\0';
  ++paramCount;
  return true;
}

void ctrlCommand::appendHelp(char *buf, size_t len) const { }

// ── CommandRegistry (unchanged) ──────────────────────────────────────────────────
void CommandRegistry::registerCommand(const ctrlCommand &cmd) {
  if (count >= MAX_COMMANDS) return;
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
  if (used == 0 && len > 0) snprintf(buf, len, "  No commands.\n");
}

void CommandRegistry::getHelpDetail(const char *cmdID, char *buf, size_t len) const {
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
  size_t used = snprintf(buf, len, "%s: %s\n", cmd->cmdID, cmd->cmdDex ? cmd->cmdDex : "");
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

CommandWithRegistry::CommandWithRegistry(const char *regID, const char *briefDesc) {
  GlobalCommandRegistry::instance().registerRegistry(regID, &registry, briefDesc);
}

void CommandWithRegistry::grabCommand(const char *commandID, const char *commandData,
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
  snprintf(originalBuf, sizeof(originalBuf), "%s %s", commandID, commandData ? commandData : "");
  if (!cmd->setParam("_original", originalBuf))
    cmd->addParam("_original", originalBuf);

  const char *p = commandData;
  if (!p) p = "";
  while (*p && isspace((unsigned char)*p)) ++p;
  if (*p && *p != '-') {
    const char *d0 = p;
    while (*p && !isspace((unsigned char)*p)) ++p;
    char tmp[PARAM_VAL_LEN];
    size_t n = (size_t)(p - d0);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, d0, n);
    tmp[n] = '\0';
    if (!cmd->setParam("_default", tmp))
      cmd->addParam("_default", tmp);
  }
  while (*p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '-') break;
    ++p;
    const char *f0 = p;
    while (*p && isalnum((unsigned char)*p)) ++p;
    char flagbuf[PARAM_KEY_LEN];
    size_t flen = (size_t)(p - f0);
    if (flen >= sizeof(flagbuf)) flen = sizeof(flagbuf) - 1;
    memcpy(flagbuf, f0, flen);
    flagbuf[flen] = '\0';
    while (*p && isspace((unsigned char)*p)) ++p;
    char valbuf[PARAM_VAL_LEN] = {};
    if (*p && *p != '-') {
      const char *v0 = p;
      while (*p && !isspace((unsigned char)*p)) ++p;
      size_t vlen = (size_t)(p - v0);
      if (vlen >= sizeof(valbuf)) vlen = sizeof(valbuf) - 1;
      memcpy(valbuf, v0, vlen);
    }
    if (!cmd->setParam(flagbuf, valbuf))
      cmd->addParam(flagbuf, valbuf);
  }
  if (cmd->funcPointer) cmd->funcPointer(cmd);
}

// ── GlobalCommandRegistry (unchanged) ───────────────────────────────────────────
GlobalCommandRegistry &GlobalCommandRegistry::instance() {
  static GlobalCommandRegistry inst;
  return inst;
}

void GlobalCommandRegistry::setBaseUrl(const char *url) { m_baseUrl = url; }

bool GlobalCommandRegistry::registerRegistry(const char *regID, CommandRegistry *reg, const char *briefDesc) {
  if (m_count >= MAX_REGISTRIES || !regID || !reg) return false;
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
  if (m_count == 0) { snprintf(buf, len, "No registries available."); return; }
  size_t used = 0;
  if (m_baseUrl) used = snprintf(buf, len, "Documentation: %s#cmd_index\n\nRegistries:\n", m_baseUrl);
  else used = snprintf(buf, len, "Documentation URL not set.\n\nRegistries:\n");
  for (uint8_t i = 0; i < m_count && used < len; ++i)
    used += snprintf(buf + used, len - used, "  %s - %s\n", m_registries[i].regID,
                     m_registries[i].briefDesc ? m_registries[i].briefDesc : "");
}

void GlobalCommandRegistry::getRegistryHelp(const char *regID, char *buf, size_t len) const {
  RegistryInfo *info = findRegistry(regID);
  if (!info) { snprintf(buf, len, "Registry '%s' not found.", regID); return; }
  size_t used = 0;
  if (m_baseUrl) used = snprintf(buf, len, "Registry %s: %s\nDocumentation: %s#%s\n\nCommands:\n",
                                 info->regID, info->briefDesc ? info->briefDesc : "", m_baseUrl, info->regID);
  else used = snprintf(buf, len, "Registry %s: %s\nDocumentation URL not set.\n\nCommands:\n",
                       info->regID, info->briefDesc ? info->briefDesc : "");
  info->registry->getHelpBrief(buf + used, len - used);
}

void GlobalCommandRegistry::getCommandHelp(const char *regID, const char *cmdID, char *buf, size_t len) const {
  RegistryInfo *info = findRegistry(regID);
  if (!info) { snprintf(buf, len, "Registry '%s' not found.", regID); return; }
  const ctrlCommand *cmd = info->registry->getCommand(cmdID);
  if (!cmd) { snprintf(buf, len, "Command '%s' not found in registry %s.", cmdID, regID); return; }
  size_t used = snprintf(buf, len, "%s: %s\n", cmd->cmdID, cmd->cmdDex ? cmd->cmdDex : "");
  if (cmd->paramCount > 0) {
    used += snprintf(buf + used, len - used, "Parameters:\n");
    for (uint8_t i = 0; i < cmd->paramCount && used < len; ++i)
      used += snprintf(buf + used, len - used, "  -%s (default: %s)\n", cmd->optParam[i].key, cmd->optParam[i].val);
  } else used += snprintf(buf + used, len - used, "No parameters.\n");
  if (m_baseUrl) snprintf(buf + used, len - used, "Full details: %s#%s", m_baseUrl, info->regID);
  else snprintf(buf + used, len - used, "Full details: URL not configured.");
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

bool MQTTdispatcher::parseCommand(const char *input, size_t inputLen, char *cmdID, size_t cmdIDLen,
                                  char *payload, size_t payloadLen) {
  if (!input || inputLen == 0 || input[0] != ':') return false;
  size_t i = 1;
  while (i < inputLen && isspace((unsigned char)input[i])) ++i;
  if (i >= inputLen) return false;
  size_t start = i;
  while (i < inputLen && !isspace((unsigned char)input[i])) ++i;
  size_t idlen = i - start;
  if (idlen >= cmdIDLen) idlen = cmdIDLen - 1;
  for (size_t k = 0; k < idlen; ++k) cmdID[k] = (char)toupper((unsigned char)input[start + k]);
  cmdID[idlen] = '\0';
  while (i < inputLen && isspace((unsigned char)input[i])) ++i;
  size_t paylen = inputLen - i;
  if (paylen >= payloadLen) paylen = payloadLen - 1;
  memcpy(payload, input + i, paylen);
  payload[paylen] = '\0';
  return true;
}

void MQTTdispatcher::on_mqtt_connected(esp_mqtt_client_handle_t client) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
  log_event(EventCode::MQTT_CONNECTED);
#endif
  static char topic_conn[64], topic_info[64];
  static bool built = false;
  if (!built) {
    snprintf(topic_conn, sizeof topic_conn, "devices/connections/%s", s_mqtt_id);
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
  esp_mqtt_client_publish(client, topic_conn, msg, n, ED_MQTT::MqttClient::MqttQoS::QOS1, true);
  int sub_msg_id = esp_mqtt_client_subscribe(client, "cmd", 0);
  esp_mqtt_client_subscribe(client, "cmd/+", 0);   // device‑specific commands
  ESP_LOGI(TAG, "Subscribed to 'cmd' and 'cmd/+'");
  static char info_buf[JSON_BUFFER_SIZE];
  build_ping_json(info_buf, sizeof info_buf);
  esp_mqtt_client_publish(client, topic_info, info_buf, strlen(info_buf),
                          ED_MQTT::MqttClient::MqttQoS::QOS1, true);

  s_ping_pending = false;
  s_ping_fail_count = 0;
  s_last_ping_msg_id = 0;
  s_mqtt_ready = true;
  s_reconnect_pending = false;
  s_mqtt_reconnect_attempts = 0;
  s_last_good_ping_time = esp_timer_get_time() / 1000000;

  esp_mqtt_client_register_event(client, MQTT_EVENT_PUBLISHED,
      [](void*, esp_event_base_t, int32_t, void* event_data) {
        MQTTdispatcher::handle_published_event((esp_mqtt_event_handle_t)event_data);
      }, nullptr);
  esp_mqtt_client_register_event(client, MQTT_EVENT_DISCONNECTED,
      [](void*, esp_event_base_t, int32_t, void*) {
        s_mqtt_ready = false;
        if (s_info_timer) xTimerStop(s_info_timer, 0);
        ESP_LOGW(TAG, "MQTT disconnected, info timer stopped");
        if (s_ping_failure_cb) s_ping_failure_cb();
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
        log_event(EventCode::MQTT_DISCONNECTED);
#endif
      }, nullptr);
  if (s_info_timer) {
    xTimerStart(s_info_timer, 0);
    ESP_LOGI(TAG, "Info timer started after MQTT connect");
  }
}

void MQTTdispatcher::on_mqtt_data(esp_mqtt_client_handle_t /*client*/,
                                  const char *topic, int topicLen,
                                  const char *data, size_t dataLen, uint32_t msgID) {
  // Filter device‑specific commands
  bool for_us = false;
  if (topicLen == 3 && strncmp(topic, "cmd", 3) == 0) for_us = true;
  else if (topicLen > 4 && strncmp(topic, "cmd/", 4) == 0) {
    const char* id_in_topic = topic + 4;
    int id_len = topicLen - 4;
    if (id_len == (int)strlen(s_mqtt_id) && strncmp(id_in_topic, s_mqtt_id, id_len) == 0) for_us = true;
  }
  if (!for_us) return;

  ESP_LOGD(TAG, "MQTT data received: topic=%.*s, data=%.*s", topicLen, topic, (int)dataLen, data);
  char cmdID[CMD_ID_LEN], payload_buf[256];
  if (parseCommand(data, dataLen, cmdID, sizeof cmdID, payload_buf, sizeof payload_buf)) {
    ESP_LOGD(TAG, "✅ Parsed colon command: '%s', payload='%s'", cmdID, payload_buf);
    if (strcmp(cmdID, "HELP") == 0 || strcmp(cmdID, "H") == 0) {
      static char helpBuf[1024];
      char arg1[CMD_ID_LEN] = {0}, arg2[CMD_ID_LEN] = {0};
      sscanf(payload_buf, "%15s %15s", arg1, arg2);
      if (arg2[0] != '\0') GlobalCommandRegistry::instance().getCommandHelp(arg1, arg2, helpBuf, sizeof(helpBuf));
      else if (arg1[0] != '\0') GlobalCommandRegistry::instance().getRegistryHelp(arg1, helpBuf, sizeof(helpBuf));
      else {
        uint8_t regCount = GlobalCommandRegistry::instance().getRegistryCount();
        if (regCount == 1) {
          const char *regID = GlobalCommandRegistry::instance().getFirstRegistryID();
          if (regID) GlobalCommandRegistry::instance().getRegistryHelp(regID, helpBuf, sizeof(helpBuf));
          else snprintf(helpBuf, sizeof(helpBuf), "Error: registry ID is null.");
        } else GlobalCommandRegistry::instance().getHelpOverview(helpBuf, sizeof(helpBuf));
      }
      esp_mqtt_client_publish(s_clHandle, "help/response", helpBuf, strlen(helpBuf), 0, 0);
      return;
    }
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
    if (strcmp(cmdID, "DUMPLOG") == 0) {
      ctrlCommand fake;
      char buf[24];
      snprintf(buf, sizeof(buf), "%lu", msgID);
      fake.addParam("_msgID", buf);
      cmd_dumplog(&fake);
      return;
    }
#endif
    if (strcmp(cmdID, "RESTART") == 0) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
      log_event(EventCode::SOFT_REBOOT);
#endif
      ESP_LOGW(TAG, "RESTART command received, rebooting in 1 second...");
      char buf[24];
      snprintf(buf, sizeof(buf), "%lu", msgID);
      ctrlCommand fake;
      fake.addParam("_msgID", buf);
      ackCommand(msgID, "RESTART", ackType::OK, "Rebooting...");
      vTaskDelay(pdMS_TO_TICKS(1000));
      esp_restart();
      return;
    }
    if (strcmp(cmdID, "PFREQ") == 0) {
      const char *arg = payload_buf;
      while (*arg && isspace((unsigned char)*arg)) ++arg;
      bool disable = false;
      TickType_t new_period_ticks = 0;
      if (*arg == 'D' || *arg == 'd') disable = true;
      else {
        int number = 0;
        if (sscanf(arg, "%d", &number) == 1) {
          if (number == 0) disable = true;
          else {
            while (*arg && isdigit((unsigned char)*arg)) ++arg;
            unsigned long multiplier = 1;
            char ch = *arg;
            if (ch == 's' || ch == 'S') ++arg;
            else if (ch == 'm' || ch == 'M') { multiplier = 60; ++arg; }
            else if (ch == 'h' || ch == 'H') { multiplier = 3600; ++arg; }
            else if (ch == 'd' || ch == 'D') { multiplier = 86400; ++arg; }
            new_period_ticks = pdMS_TO_TICKS((unsigned long)number * multiplier * 1000);
          }
        } else { ESP_LOGW(TAG, "PFREQ: Invalid argument '%s'", arg); return; }
      }
      if (s_info_timer) {
        if (disable) {
          if (xTimerStop(s_info_timer, 0) == pdPASS) {
            ESP_LOGI(TAG, "PFREQ: Periodic ping disabled");
            if (s_clHandle) esp_mqtt_client_publish(s_clHandle, "ack", "Ping disabled", 0, 0, 0);
          } else ESP_LOGE(TAG, "PFREQ: Failed to stop timer");
        } else {
          if (xTimerChangePeriod(s_info_timer, new_period_ticks, 0) == pdPASS) {
            ESP_LOGI(TAG, "PFREQ: Ping interval changed to %lu ms", (unsigned long)(new_period_ticks * portTICK_PERIOD_MS));
            xTimerStart(s_info_timer, 0);
            if (s_clHandle) {
              char ack_msg[64];
              snprintf(ack_msg, sizeof(ack_msg), "Ping interval set to %lu ms", (unsigned long)(new_period_ticks * portTICK_PERIOD_MS));
              esp_mqtt_client_publish(s_clHandle, "ack", ack_msg, 0, 0, 0);
            }
          } else ESP_LOGE(TAG, "PFREQ: Failed to change timer period");
        }
      } else ESP_LOGW(TAG, "PFREQ: Info timer not initialised");
      return;
    }
    for (uint8_t i = 0; i < s_subscriber_count; ++i)
      if (s_subscribers[i]) s_subscribers[i]->grabCommand(cmdID, payload_buf, strlen(payload_buf), msgID);
    return;
  } else ESP_LOGW(TAG, "❌ Failed to parse as colon command (does it start with ':'?)");
  handleCommandObject(data, dataLen, msgID);
}

void MQTTdispatcher::handleCommandObject(const char *json, size_t /*jsonLen*/, uint32_t cmdID) {
  ED_S_JSON::StaticJson decoder(json);
  if (decoder.isValid()) {
    const char *cmd = decoder.getString("cmd");
    const char *data = decoder.getString("data");
    if (cmd && data) {
      for (uint8_t i = 0; i < s_subscriber_count; ++i)
        if (s_subscribers[i]) s_subscribers[i]->grabCommand(cmd, data, strlen(data), cmdID);
      return;
    }
  }
  const char *p = json;
  while ((p = strstr(p, "\"cmd\"")) != nullptr) {
    const char *cmd_start = strchr(p, ':');
    if (!cmd_start) break;
    cmd_start = strchr(cmd_start, '"');
    if (!cmd_start) break;
    cmd_start++;
    const char *cmd_end = strchr(cmd_start, '"');
    if (!cmd_end) break;
    const char *data_start = strstr(cmd_end, "\"data\"");
    if (!data_start) break;
    data_start = strchr(data_start, ':');
    if (!data_start) break;
    data_start = strchr(data_start, '"');
    if (!data_start) break;
    data_start++;
    const char *data_end = strchr(data_start, '"');
    if (!data_end) break;
    char cmd_buf[CMD_ID_LEN], data_buf[256];
    size_t cmd_len = cmd_end - cmd_start;
    if (cmd_len >= sizeof(cmd_buf)) cmd_len = sizeof(cmd_buf) - 1;
    strncpy(cmd_buf, cmd_start, cmd_len); cmd_buf[cmd_len] = '\0';
    size_t data_len = data_end - data_start;
    if (data_len >= sizeof(data_buf)) data_len = sizeof(data_buf) - 1;
    strncpy(data_buf, data_start, data_len); data_buf[data_len] = '\0';
    for (uint8_t i = 0; i < s_subscriber_count; ++i)
      if (s_subscribers[i]) s_subscribers[i]->grabCommand(cmd_buf, data_buf, data_len, cmdID);
    p = data_end + 1;
    while (*p && *p != ',' && *p != '}') ++p;
    if (*p == ',') ++p;
  }
}

void MQTTdispatcher::ackCommand(int64_t reqMsgID, const char *commandID, ackType ackResult, const char *originalCommand) {
  if (!s_mqtt) { ESP_LOGW(TAG, "ackCommand: MQTT client not available"); return; }
  static char topic_ack[64];
  static bool built = false;
  if (!built) {
    snprintf(topic_ack, sizeof topic_ack, "ack/%s", s_mqtt_id);
    built = true;
  }
  char ackbuf[256];
  const char *display = (originalCommand && originalCommand[0]) ? originalCommand : commandID;
  int n = snprintf(ackbuf, sizeof ackbuf, "[%s] %s", display ? display : "?", ackResult == ackType::OK ? "OK" : "FAIL");
  if (n < 0) n = 0;
  if (n >= (int)sizeof ackbuf) n = (int)sizeof ackbuf - 1;
  bool ok = s_mqtt->publish(topic_ack, ackbuf, 1, false);
  if (!ok) ESP_LOGE(TAG, "ackCommand publish failed");
}

void MQTTdispatcher::build_ping_json(char *buf, size_t len) {
  ED_S_JSON::StaticJson doc;
  doc.beginObject();
  doc.addString("dDGT", "DTF");
  doc.addString("dS", "N");
  doc.addString("d_UPT", ED_SYS::ESP_std::Runtime::uptime());
  doc.beginArray("diagnostics");
  for (uint8_t i = 0; i < s_json_provider_count; ++i) {
    if (s_json_providers[i]) {
      doc.beginObject();
      s_json_providers[i](doc);
      doc.endObject();
    }
  }
  doc.endArray();
  doc.endObject();
  const char *json_str = doc.toString();
  size_t needed = strlen(json_str) + 1;
  if (needed <= len) memcpy(buf, json_str, needed);
  else {
    strncpy(buf, json_str, len - 1);
    buf[len - 1] = '\0';
    ESP_LOGW(TAG, "build_ping_json truncated (%zu > %zu)", needed - 1, len - 1);
  }
}

void MQTTdispatcher::T_info_timer_callback(TimerHandle_t /*handle*/) {
  if (s_info_task_handle) xTaskNotifyGive(s_info_task_handle);
}

void MQTTdispatcher::info_publisher_task(void *) {
  for (;;) { ulTaskNotifyTake(pdTRUE, portMAX_DELAY); publishInfo(); }
}

#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
void MQTTdispatcher::log_event(EventCode code) {
    if (s_log_magic != LOG_MAGIC) {
        s_log_magic = LOG_MAGIC;
        s_log_count = 0;
        s_log_head = 0;
        s_log_start_idx = 0;
        s_log_base_time = 0;
    }

    uint32_t now = esp_timer_get_time() / 1000;
    if (s_log_count == 0) {
        s_log_base_time = now;
        s_log_start_idx = 0;
        s_log_head = 0;
        s_log_buffer[0] = static_cast<uint8_t>(code);
        s_log_buffer[1] = 0;
        s_log_buffer[2] = 0;
        s_log_count = 1;
        s_log_head = 1;
        return;
    }

    // Compute absolute time of last stored event
    uint32_t last_abs = s_log_base_time;
    uint16_t idx = s_log_start_idx;
    for (uint16_t i = 1; i < s_log_count; ++i) {
        uint16_t d = s_log_buffer[idx * 3 + 1] | (s_log_buffer[idx * 3 + 2] << 8);
        last_abs += d;
        idx = (idx + 1) % 682;
    }
    uint32_t delta = now - last_abs;
    if (delta > 65535) delta = 65535;
    uint16_t write_idx = s_log_head;
    s_log_buffer[write_idx * 3] = static_cast<uint8_t>(code);
    s_log_buffer[write_idx * 3 + 1] = delta & 0xFF;
    s_log_buffer[write_idx * 3 + 2] = (delta >> 8) & 0xFF;
    s_log_head = (write_idx + 1) % 682;
    if (s_log_count < 682) s_log_count++;
    else {
        uint16_t old_start = s_log_start_idx;
        uint16_t old_delta = s_log_buffer[old_start * 3 + 1] | (s_log_buffer[old_start * 3 + 2] << 8);
        s_log_base_time += old_delta;
        s_log_start_idx = (s_log_start_idx + 1) % 682;
    }
}

void MQTTdispatcher::cmd_dumplog(ctrlCommand* cmd) {
    static char line_buf[80];
    static char out_buf[1400];
    int total_events = s_log_count;
    if (total_events == 0) {
        if (s_mqtt && s_clHandle) {
            char topic[64];
            snprintf(topic, sizeof(topic), "devices/%s/dumplog", s_mqtt_id);
            esp_mqtt_client_publish(s_clHandle, topic, "No events logged.\n", 0, 0, 0);
        }
        const char* msgid = cmd ? cmd->getParam("_msgID") : nullptr;
        if (msgid && msgid[0]) {
            int64_t id = atoll(msgid);
            ackCommand(id, "DUMPLOG", ackType::OK, "No events");
        }
        return;
    }

    uint16_t start = s_log_start_idx;
    uint32_t abs_time = s_log_base_time;
    int part = 1;
    int pos = 0;

    // First header
    int header_len = snprintf(line_buf, sizeof(line_buf),
        "=== %u events (oldest at %u.%03u sec uptime) ===\n",
        total_events, (unsigned int)(abs_time / 1000), (unsigned int)(abs_time % 1000));
    if (header_len > 0 && header_len < (int)sizeof(out_buf)) {
        memcpy(out_buf, line_buf, header_len);
        pos = header_len;
    }

    for (uint16_t i = 0; i < total_events; ++i) {
        uint16_t idx = (start + i) % 682;
        uint8_t code = s_log_buffer[idx * 3];
        if (code == 0) continue; // skip uninitialised
        uint16_t delta = s_log_buffer[idx * 3 + 1] | (s_log_buffer[idx * 3 + 2] << 8);
        if (i > 0) abs_time += delta;
        uint32_t sec = abs_time / 1000, ms = abs_time % 1000;
        const char* msg = event_code_to_string(code);
        int len = snprintf(line_buf, sizeof(line_buf), "[%3u.%03u] %s\n", (unsigned int)sec, (unsigned int)ms, msg);
        if (len <= 0) continue;
        if (pos + len >= (int)sizeof(out_buf)) {
            if (s_mqtt && s_clHandle) {
                char topic[64];
                snprintf(topic, sizeof(topic), "devices/%s/dumplog/%d", s_mqtt_id, part);
                esp_mqtt_client_publish(s_clHandle, topic, out_buf, pos, 0, 0);
            }
            part++;
            pos = 0;
            if (i < total_events - 1) {
                int page_len = snprintf(line_buf, sizeof(line_buf), "=== Page %d ===\n", part);
                if (page_len > 0 && page_len < (int)sizeof(out_buf)) {
                    memcpy(out_buf, line_buf, page_len);
                    pos = page_len;
                }
            }
        }
        memcpy(out_buf + pos, line_buf, len);
        pos += len;
    }
    if (pos > 0 && s_mqtt && s_clHandle) {
        char topic[64];
        snprintf(topic, sizeof(topic), "devices/%s/dumplog/%d", s_mqtt_id, part);
        esp_mqtt_client_publish(s_clHandle, topic, out_buf, pos, 0, 0);
    }
    const char* msgid = cmd ? cmd->getParam("_msgID") : nullptr;
    if (msgid && msgid[0]) {
        int64_t id = atoll(msgid);
        ackCommand(id, "DUMPLOG", ackType::OK, "Log dumped in multiple messages");
    }
}
#endif

void MQTTdispatcher::publishInfo() {
  if (!s_mqtt_ready) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
    log_event(EventCode::INFO_PING_SKIPPED);
#endif
    ESP_LOGD(TAG, "MQTT not ready, skipping publish");
    return;
  }
  if (s_ping_pending) {
    s_ping_fail_count++;
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
    log_event(EventCode::MISSING_PUBACK);
#endif
    ESP_LOGW(TAG, "Missing PUBACK for msgID=%d, fail count=%d/%d", s_last_ping_msg_id, s_ping_fail_count, PING_MAX_FAILURES);
    if (s_ping_fail_count >= PING_MAX_FAILURES) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
      log_event(EventCode::TOO_MANY_MISSED);
      log_event(EventCode::FORCE_RECONNECT);
#endif
      if (s_ping_failure_cb) s_ping_failure_cb();
      s_mqtt_ready = false;
      if (s_info_timer) xTimerStop(s_info_timer, 0);
      if (!s_reconnect_pending) {
        s_reconnect_pending = true;
        s_mqtt_reconnect_attempts++;
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
        log_event(EventCode::RECONNECT_ATTEMPT);
#endif
        ESP_LOGW(TAG, "MQTT reconnect attempt #%d", s_mqtt_reconnect_attempts);
        if (s_mqtt_reconnect_attempts >= MQTT_RECONNECT_THRESHOLD) {
          int64_t now = esp_timer_get_time() / 1000000;
          if (now - s_last_wifi_reconnect_time >= WIFI_RECONNECT_COOLDOWN_SEC) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
            log_event(EventCode::THRESHOLD_REACHED);
            log_event(EventCode::WIFI_RECONNECT);
#endif
            ED_wifi::WiFiService::forceReconnect();
            s_last_wifi_reconnect_time = now;
            s_mqtt_reconnect_attempts = 0;
          } else {
            int64_t remaining = WIFI_RECONNECT_COOLDOWN_SEC - (now - s_last_wifi_reconnect_time);
            ESP_LOGW(TAG, "WiFi cooldown active (%lld sec remaining), skipping WiFi reset", remaining);
          }
        }
        ED_MQTT::MqttClient::forceReconnect();
      } else ESP_LOGW(TAG, "Reconnect already pending, skipping duplicate call");
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
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
    log_event(EventCode::PUBLISH_ERROR);
#endif
    ESP_LOGE(TAG, "publishInfo send error (err=%d)", msg_id);
    s_ping_fail_count++;
    if (s_ping_fail_count >= PING_MAX_FAILURES) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
      log_event(EventCode::TOO_MANY_MISSED);
      log_event(EventCode::FORCE_RECONNECT);
#endif
      if (s_ping_failure_cb) s_ping_failure_cb();
      s_mqtt_ready = false;
      if (s_info_timer) xTimerStop(s_info_timer, 0);
      if (!s_reconnect_pending) {
        s_reconnect_pending = true;
        s_mqtt_reconnect_attempts++;
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
        log_event(EventCode::RECONNECT_ATTEMPT);
#endif
        ESP_LOGW(TAG, "MQTT reconnect attempt #%d", s_mqtt_reconnect_attempts);
        if (s_mqtt_reconnect_attempts >= MQTT_RECONNECT_THRESHOLD) {
          int64_t now = esp_timer_get_time() / 1000000;
          if (now - s_last_wifi_reconnect_time >= WIFI_RECONNECT_COOLDOWN_SEC) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
            log_event(EventCode::THRESHOLD_REACHED);
            log_event(EventCode::WIFI_RECONNECT);
#endif
            ED_wifi::WiFiService::forceReconnect();
            s_last_wifi_reconnect_time = now;
            s_mqtt_reconnect_attempts = 0;
          } else {
            int64_t remaining = WIFI_RECONNECT_COOLDOWN_SEC - (now - s_last_wifi_reconnect_time);
            ESP_LOGW(TAG, "WiFi cooldown active (%lld sec remaining), skipping WiFi reset", remaining);
          }
        }
        ED_MQTT::MqttClient::forceReconnect();
      } else ESP_LOGW(TAG, "Reconnect already pending, skipping duplicate call");
      s_ping_fail_count = 0;
      s_ping_pending = false;
    } else if (s_ping_failure_cb) s_ping_failure_cb();
  } else {
    s_last_ping_msg_id = msg_id;
    s_ping_pending = true;
    ESP_LOGD(TAG, "Info ping sent, msgID=%d", msg_id);
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
    log_event(EventCode::INFO_PING_SENT);
#endif
  }
}

void MQTTdispatcher::handle_published_event(esp_mqtt_event_handle_t event) {
    if (!s_mqtt_ready) return;
    if (!event || !s_ping_pending) return;
    if (event->msg_id == s_last_ping_msg_id) {
        ESP_LOGD(TAG, "PUBACK received for msgID=%d", event->msg_id);
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
        log_event(EventCode::PUBACK_RECEIVED);
#endif
        s_ping_pending = false;
        s_last_good_ping_time = esp_timer_get_time() / 1000000;
        if (s_ping_fail_count > 0) {
            ESP_LOGI(TAG, "Resetting fail count from %d to 0", s_ping_fail_count);
            s_ping_fail_count = 0;
        }
        if (s_mqtt_reconnect_attempts > 0) s_mqtt_reconnect_attempts = 0;
        if (s_ping_success_cb) s_ping_success_cb();
        if (s_reconnect_pending) s_reconnect_pending = false;
    }
}

void MQTTdispatcher::registerPingSuccessCallback(PingSuccessCallback cb) { s_ping_success_cb = cb; }
void MQTTdispatcher::registerPingFailureCallback(PingFailureCallback cb) { s_ping_failure_cb = cb; }
void MQTTdispatcher::resetMqttReconnectAttempts() { s_mqtt_reconnect_attempts = 0; }

static void dead_man_task(void *arg) {
    const int64_t TIMEOUT_MQTT = 600, TIMEOUT_WIFI = 120, TIMEOUT_FULL = 120;
    enum { STATE_OK, STATE_MQTT_RECONNECTED, STATE_WIFI_RECONNECTED } state = STATE_OK;
    int64_t last_attempt = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        int64_t now = esp_timer_get_time() / 1000000;
        if (MQTTdispatcher::s_last_good_ping_time == 0) continue;
        if (now - MQTTdispatcher::s_last_good_ping_time > TIMEOUT_MQTT) {
            if (state == STATE_OK) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
                MQTTdispatcher::log_event(MQTTdispatcher::EventCode::DEAD_MAN_MQTT);
#else
                ESP_LOGW(TAG, "Dead‑man: no PUBACK for 10 min, MQTT reconnect");
#endif
                ED_MQTT::MqttClient::forceReconnect();
                state = STATE_MQTT_RECONNECTED;
                last_attempt = now;
            } else if (state == STATE_MQTT_RECONNECTED && (now - last_attempt) > TIMEOUT_WIFI) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
                MQTTdispatcher::log_event(MQTTdispatcher::EventCode::DEAD_MAN_WIFI);
#else
                ESP_LOGW(TAG, "Dead‑man: MQTT reconnect failed, forcing WiFi stack restart");
#endif
                ED_wifi::WiFiService::forceReconnect();
                state = STATE_WIFI_RECONNECTED;
                last_attempt = now;
            } else if (state == STATE_WIFI_RECONNECTED && (now - last_attempt) > TIMEOUT_FULL) {
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
                MQTTdispatcher::log_event(MQTTdispatcher::EventCode::DEAD_MAN_RESTART);
#else
                ESP_LOGE(TAG, "Dead‑man: WiFi restart failed, restarting system");
#endif
                esp_restart();
            }
        } else state = STATE_OK;
    }
}

esp_err_t MQTTdispatcher::initialize(esp_mqtt_client_config_t *config) {
  strncpy(s_mqtt_id, ED_SYS::ESP_std::Device::mqttName(), sizeof s_mqtt_id - 1);
  s_config = config;
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
  if (s_log_magic != LOG_MAGIC) {
    s_log_magic = LOG_MAGIC;
    s_log_count = 0;
    s_log_head = 0;
    s_log_start_idx = 0;
    s_log_base_time = 0;
    ESP_LOGI(TAG, "Fresh persistent log (first boot)");
  } else {
    ESP_LOGI(TAG, "Persistent log loaded from RTC memory, %u events", s_log_count);
  }
#endif
  s_info_timer = xTimerCreate("info_loop", pdMS_TO_TICKS(10000), pdTRUE, nullptr, T_info_timer_callback);
  if (s_info_timer) ESP_LOGI(TAG, "Info timer created");
  else ESP_LOGE(TAG, "Info timer creation failed");
  xTaskCreate(info_publisher_task, "info_pub", 8192, nullptr, 5, &s_info_task_handle);
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
  strncpy(s_cached_ip, ED_SYS::ESP_std::Device::curIP(), sizeof(s_cached_ip) - 1);
  s_cached_ip[sizeof(s_cached_ip) - 1] = '\0';
  ESP_LOGI(TAG, "IP ready — creating MQTT client");
#ifdef ED_MQTT_DISPATCHER_ENABLE_PERSISTENT_LOG
  log_event(EventCode::IP_READY);
#endif
  s_mqtt = ED_MQTT::MqttClient::create(s_config);
  if (!s_mqtt) { ESP_LOGE(TAG, "MqttClient::create failed"); return; }
  s_clHandle = s_mqtt->getHandle();
  s_mqtt_ready = false;
  s_mqtt->registerConnectedCallback(on_mqtt_connected);
  s_mqtt->registerDataCallback(on_mqtt_data);
  ESP_LOGI(TAG, "MQTT client created, connection will happen asynchronously");
}

void MQTTdispatcher::registerJsonFieldProvider(JsonFieldProvider provider) {
  if (!provider) { ESP_LOGW(TAG, "Null JSON provider ignored"); return; }
  if (s_json_provider_count >= MAX_JSON_PROVIDERS) { ESP_LOGE(TAG, "Too many JSON providers, max=%d", MAX_JSON_PROVIDERS); return; }
  s_json_providers[s_json_provider_count++] = provider;
}

} // namespace ED_MQTT_dispatcher