#include "ED_MQTT_dispatcher.h"
#include "ED_S_JSON.h"
#include "ED_sys.h"
#include "ED_wifi.h"
#include "esp_log.h"
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

//-------------------------------------------------------------

// ── ctrlCommand helpers ─────────────────────────────────────────────
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

// ── CommandRegistry ──────────────────────────────────────────────────
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

CommandWithRegistry::CommandWithRegistry(const char* regID, const char* briefDesc) {
    GlobalCommandRegistry::instance().registerRegistry(regID, &registry, briefDesc);
}

// ── CommandWithRegistry::grabCommand (injects _msgID and _original) ──
void CommandWithRegistry::grabCommand(const char *commandID,
                                      const char *commandData,
                                      size_t /*dataLen*/,
                                      uint32_t msgID) {

    ctrlCommand *cmd = registry.getCommand(commandID);
    if (!cmd) {
        ESP_LOGW("CmdReg", "Command '%s' not found", commandID);
        return;
    }

    // Convert msgID to string safely
    char msgIDstr[24];
    int len = snprintf(msgIDstr, sizeof(msgIDstr), "%lu", (uint32_t)msgID);
    if (len <= 0 || len >= (int)sizeof(msgIDstr)) {
        ESP_LOGE("CmdReg", "Failed to convert msgID to string, using fallback '0'");
        strcpy(msgIDstr, "0");
    }

    // Inject _msgID (use a dedicated key that won't be overwritten by flag parsing)
    // First try to setParam, if fails then addParam
    if (!cmd->setParam("_msgID", msgIDstr)) {
        ESP_LOGI("CmdReg", "Adding _msgID param (setParam failed)");
        cmd->addParam("_msgID", msgIDstr);
    }

    // Also store a second copy under a different key as a backup
    if (!cmd->setParam("_msgID_raw", msgIDstr)) {
        cmd->addParam("_msgID_raw", msgIDstr);
    }

    // Verify that the parameter was stored correctly
    const char *check = cmd->getParam("_msgID");

    // Inject the original full command string
    char originalBuf[PARAM_VAL_LEN];
    snprintf(originalBuf, sizeof(originalBuf), "%s %s", commandID, commandData ? commandData : "");
    if (!cmd->setParam("_original", originalBuf))
        cmd->addParam("_original", originalBuf);

    // Parse colon‑format flags: -key value
    const char *p = commandData;
    if (!p) p = "";

    // Skip leading whitespace
    while (*p && isspace((unsigned char)*p)) ++p;

    // First token = default value (if not a flag)
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

    // Remaining tokens: -key [value]
    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p != '-') break;
        ++p; // skip '-'

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

        // Use addParam because the flag may not exist yet; we don't want to overwrite existing values.
        // But setParam first to avoid duplicate if it already exists.
        if (!cmd->setParam(flagbuf, valbuf))
            cmd->addParam(flagbuf, valbuf);
    }

    // Final sanity check for _msgID
    const char *finalMsgID = cmd->getParam("_msgID");

    // Execute the command
    if (cmd->funcPointer)
        cmd->funcPointer(cmd);
}

// ── GlobalCommandRegistry (singleton) ───────────────────────────────
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
    used = snprintf(buf, len, "Documentation: %s#cmd_index\n\nRegistries:\n", m_baseUrl);
  } else {
    used = snprintf(buf, len, "Documentation URL not set.\n\nRegistries:\n");
  }

  for (uint8_t i = 0; i < m_count && used < len; ++i) {
    used += snprintf(buf + used, len - used, "  %s - %s\n", m_registries[i].regID,
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
    used = snprintf(buf, len, "Registry %s: %s\nDocumentation: %s#%s\n\nCommands:\n",
                    info->regID, info->briefDesc ? info->briefDesc : "", m_baseUrl,
                    info->regID);
  } else {
    used = snprintf(buf, len, "Registry %s: %s\nDocumentation URL not set.\n\nCommands:\n",
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
    snprintf(buf + used, len - used, "Full details: %s#%s", m_baseUrl, info->regID);
  } else {
    snprintf(buf + used, len - used, "Full details: URL not configured.");
  }
}

// ── MQTTdispatcher implementation ────────────────────────────────────

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
  static char topic_conn[64];
  static char topic_info[64];
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
  if (n < 0)
    n = 0;

  esp_mqtt_client_publish(client, topic_conn, msg, n,
                          ED_MQTT::MqttClient::MqttQoS::QOS1, true);
  int sub_msg_id = esp_mqtt_client_subscribe(client, "cmd", 0);
  ESP_LOGI(TAG, "Subscribed to 'cmd', msg_id=%d", sub_msg_id);

  static char info_buf[JSON_BUFFER_SIZE];
  build_ping_json(info_buf, sizeof info_buf);
  esp_mqtt_client_publish(client, topic_info, info_buf, strlen(info_buf),
                          ED_MQTT::MqttClient::MqttQoS::QOS1, true);
}

void MQTTdispatcher::on_mqtt_data(esp_mqtt_client_handle_t /*client*/,
                                  const char *topic, int topicLen,
                                  const char *data, size_t dataLen,
                                  uint32_t msgID) {
    ESP_LOGD(TAG, "MQTT data received: topic=%.*s, data=%.*s", topicLen, topic,
             (int)dataLen, data);

    char cmdID[CMD_ID_LEN];
    char payload_buf[256];

    if (parseCommand(data, dataLen, cmdID, sizeof cmdID, payload_buf,
                     sizeof payload_buf)) {
        ESP_LOGD(TAG, "✅ Parsed colon command: '%s', payload='%s'", cmdID, payload_buf);

        // HELP command handling
if (strcmp(cmdID, "HELP") == 0 || strcmp(cmdID, "H") == 0) {
    static char helpBuf[1024];
    char arg1[CMD_ID_LEN] = {0};
    char arg2[CMD_ID_LEN] = {0};
    sscanf(payload_buf, "%15s %15s", arg1, arg2);

    if (arg2[0] != '\0') {
        // HELP <registry> <command>
        GlobalCommandRegistry::instance().getCommandHelp(arg1, arg2, helpBuf, sizeof(helpBuf));
    }
    else if (arg1[0] != '\0') {
        // HELP <registry>
        GlobalCommandRegistry::instance().getRegistryHelp(arg1, helpBuf, sizeof(helpBuf));
    }
    else {
        // No arguments: decide based on number of registries
        uint8_t regCount = GlobalCommandRegistry::instance().getRegistryCount();
        if (regCount == 1) {
            // Only one registry – show its help directly
            const char* regID = GlobalCommandRegistry::instance().getFirstRegistryID();
            if (regID) {
                GlobalCommandRegistry::instance().getRegistryHelp(regID, helpBuf, sizeof(helpBuf));
            } else {
                snprintf(helpBuf, sizeof(helpBuf), "Error: registry ID is null.");
            }
        } else {
            // Multiple registries – show overview
            GlobalCommandRegistry::instance().getHelpOverview(helpBuf, sizeof(helpBuf));
        }
    }

    esp_mqtt_client_publish(s_clHandle, "help/response", helpBuf, strlen(helpBuf), 0, 0);
    return;
}

        // ── PFREQ command: configure periodic ping interval ────────
        if (strcmp(cmdID, "PFREQ") == 0) {
            const char *arg = payload_buf;
            while (*arg && isspace((unsigned char)*arg)) ++arg;

            bool disable = false;
            TickType_t new_period_ticks = 0;

            if (*arg == 'D' || *arg == 'd') {
                disable = true;
            } else {
                int number = 0;
                if (sscanf(arg, "%d", &number) == 1) {
                    if (number == 0) {
                        disable = true;
                    } else {
                        // move past digits
                        while (*arg && isdigit((unsigned char)*arg)) ++arg;
                        unsigned long multiplier = 1;   // default = seconds
                        char ch = *arg;
                        if (ch == 's' || ch == 'S') {
                            ++arg;
                        } else if (ch == 'm' || ch == 'M') {
                            multiplier = 60;
                            ++arg;
                        } else if (ch == 'h' || ch == 'H') {
                            multiplier = 3600;
                            ++arg;
                        } else if (ch == 'd' || ch == 'D') {
                            multiplier = 86400;
                            ++arg;
                        }
                        unsigned long total_sec = (unsigned long)number * multiplier;
                        new_period_ticks = pdMS_TO_TICKS(total_sec * 1000);
                    }
                } else {
                    ESP_LOGW(TAG, "PFREQ: Invalid argument '%s'", arg);
                    return;
                }
            }

            if (s_info_timer) {
                if (disable) {
                    if (xTimerStop(s_info_timer, 0) == pdPASS) {
                        ESP_LOGI(TAG, "PFREQ: Periodic ping disabled");
                        if (s_clHandle) {
                            esp_mqtt_client_publish(s_clHandle, "ack", "Ping disabled", 0, 0, 0);
                        }
                    } else {
                        ESP_LOGE(TAG, "PFREQ: Failed to stop timer");
                    }
                } else {
                    if (xTimerChangePeriod(s_info_timer, new_period_ticks, 0) == pdPASS) {
                        ESP_LOGI(TAG, "PFREQ: Ping interval changed to %lu ms",
                                 (unsigned long)(new_period_ticks * portTICK_PERIOD_MS));
                        // ensure timer is running
                        xTimerStart(s_info_timer, 0);
                        if (s_clHandle) {
                            char ack_msg[64];
                            snprintf(ack_msg, sizeof(ack_msg),
                                     "Ping interval set to %lu ms",
                                     (unsigned long)(new_period_ticks * portTICK_PERIOD_MS));
                            esp_mqtt_client_publish(s_clHandle, "ack", ack_msg, 0, 0, 0);
                        }
                    } else {
                        ESP_LOGE(TAG, "PFREQ: Failed to change timer period");
                    }
                }
            } else {
                ESP_LOGW(TAG, "PFREQ: Info timer not initialised");
            }
            return;   // command handled, don’t pass to subscribers
        }

        // Normal colon command – dispatch to subscribers
        ESP_LOGD(TAG, "Subscriber count: %d", s_subscriber_count);
        for (uint8_t i = 0; i < s_subscriber_count; ++i) {
            if (s_subscribers[i]) {
                ESP_LOGD(TAG, "Dispatching to subscriber %d", i);
                s_subscribers[i]->grabCommand(cmdID, payload_buf, strlen(payload_buf),
                                              msgID);
            } else {
                ESP_LOGD(TAG, "Subscriber %d is NULL", i);
            }
        }
        if (s_subscriber_count == 0) {
            ESP_LOGW(TAG, "No subscribers registered - command ignored");
        }
        return;
    } else {
        ESP_LOGW(TAG, "❌ Failed to parse as colon command (does it start with ':'?)");
    }

    // Try JSON format
    handleCommandObject(data, dataLen, msgID);
}

void MQTTdispatcher::handleCommandObject(const char *json, size_t /*jsonLen*/,
                                         uint32_t cmdID) {
  // ----- Step 1: try as single object with "cmd" and "data" -----
  ED_S_JSON::StaticJson decoder(json);
  if (decoder.isValid()) {
    const char *cmd = decoder.getString("cmd");
    const char *data = decoder.getString("data");
    if (cmd && data) {
      for (uint8_t i = 0; i < s_subscriber_count; ++i)
        if (s_subscribers[i])
          s_subscribers[i]->grabCommand(cmd, data, strlen(data), cmdID);
      return;
    }
  }

  // ----- Step 2: try as array of objects { "cmd":..., "data":... } -----
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

    char cmd_buf[CMD_ID_LEN];
    size_t cmd_len = cmd_end - cmd_start;
    if (cmd_len >= sizeof(cmd_buf)) cmd_len = sizeof(cmd_buf) - 1;
    strncpy(cmd_buf, cmd_start, cmd_len);
    cmd_buf[cmd_len] = '\0';

    char data_buf[256];
    size_t data_len = data_end - data_start;
    if (data_len >= sizeof(data_buf)) data_len = sizeof(data_buf) - 1;
    strncpy(data_buf, data_start, data_len);
    data_buf[data_len] = '\0';

    for (uint8_t i = 0; i < s_subscriber_count; ++i) {
      if (s_subscribers[i])
        s_subscribers[i]->grabCommand(cmd_buf, data_buf, data_len, cmdID);
    }

    p = data_end + 1;
    while (*p && *p != ',' && *p != '}') ++p;
    if (*p == ',') ++p;
  }
}

void MQTTdispatcher::ackCommand(int64_t reqMsgID, const char *commandID,
                                ackType ackResult, const char *originalCommand) {
    if (!s_mqtt) {
        ESP_LOGW(TAG, "ackCommand: MQTT client not available");
        return;
    }

    static char topic_ack[64];
    static bool built = false;
    if (!built) {
        snprintf(topic_ack, sizeof topic_ack, "ack/%s", s_mqtt_id);
        built = true;
    }

    char ackbuf[256];
    const char *display = (originalCommand && originalCommand[0]) ? originalCommand : commandID;
    int n = snprintf(ackbuf, sizeof ackbuf, "[%s] %s",
                     display ? display : "?",
                     ackResult == ackType::OK ? "OK" : "FAIL");
    if (n < 0) n = 0;
    if (n >= (int)sizeof ackbuf) n = (int)sizeof ackbuf - 1;

    bool ok = s_mqtt->publish(topic_ack, ackbuf, 1, false);   // QoS1, not retained
    if (!ok) {
        ESP_LOGE(TAG, "ackCommand publish failed");
    }
}

void MQTTdispatcher::build_ping_json(char *buf, size_t len) {
    ED_S_JSON::StaticJson doc;

    // Root object
    doc.beginObject();

    // Core fields
    doc.addString("dDGT", "DTF");
    doc.addString("dS", "N");
    doc.addString("d_UPT", ED_SYS::ESP_std::Runtime::uptime());

    // Diagnostics array
    doc.beginArray("diagnostics");

    // Call each registered provider – each adds one object to the array
    for (uint8_t i = 0; i < s_json_provider_count; ++i) {
        if (s_json_providers[i]) {
            // Start an anonymous object inside the array
            doc.beginObject();
            // Provider adds its fields to this object
            s_json_providers[i](doc);
            // Close the object
            doc.endObject();
        }
    }

    doc.endArray();   // end diagnostics array
    doc.endObject();  // end root object

    // Copy the generated JSON to the output buffer
    const char* json_str = doc.toString();
    size_t needed = strlen(json_str) + 1;

    if (needed <= len) {
        memcpy(buf, json_str, needed);
    } else {
        // Truncate safely if output buffer is too small (should not happen)
        strncpy(buf, json_str, len - 1);
        buf[len - 1] = '\0';
        ESP_LOGW(TAG, "build_ping_json truncated (%zu > %zu)", needed - 1, len - 1);
    }
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

    // Use MqttClient wrapper to get client‑id property automatically
    bool ok = s_mqtt->publish(topic_info, buf, 0, true);   // QoS0, retain

    if (!ok)
        ESP_LOGE(TAG, "publishInfo failed");
    // else
    //     ESP_LOGI(TAG, "publishInfo ok");
}

esp_err_t MQTTdispatcher::initialize(esp_mqtt_client_config_t *config) {
  strncpy(s_mqtt_id, ED_SYS::ESP_std::Device::mqttName(), sizeof s_mqtt_id - 1);
  s_config = config;

  // ── 10-second timer (default) ─────────────────────────────────
  s_info_timer = xTimerCreate("info_loop", pdMS_TO_TICKS(10000), pdTRUE,
                              nullptr, T_info_timer_callback);
  if (!s_info_timer) {
    ESP_LOGE(TAG, "xTimerCreate failed");
    return ESP_FAIL;
  }

  xTaskCreate(info_publisher_task, "info_pub", 8192, nullptr, 5,
              &s_info_task_handle);

  ESP_LOGI(TAG, "initialized, waiting for IP before starting MQTT");
  return ESP_OK;
}

esp_err_t MQTTdispatcher::run() {
  ED_wifi::WiFiService::subscribeToIPReady(on_ip_ready);
  ESP_LOGI(TAG, "run() — MQTT will start once IP is ready");
  return ESP_OK;
}

void MQTTdispatcher::on_ip_ready() {
   // Cache IP for later use in info publisher
    strncpy(s_cached_ip, ED_SYS::ESP_std::Device::curIP(), sizeof(s_cached_ip) - 1);
    s_cached_ip[sizeof(s_cached_ip) - 1] = '\0';

  ESP_LOGI(TAG, "IP ready — creating MQTT client");
  s_mqtt = ED_MQTT::MqttClient::create(s_config);
  if (!s_mqtt) {
    ESP_LOGE(TAG, "MqttClient::create failed");
    return;
  }
  s_clHandle = s_mqtt->getHandle();

  ESP_LOGI(TAG, "Waiting 3 seconds for MQTT connection...");
  vTaskDelay(pdMS_TO_TICKS(3000));

  int sub_id = esp_mqtt_client_subscribe(s_clHandle, "cmd", 0);
  ESP_LOGI(TAG, "Direct subscription to 'cmd', msg_id=%d", sub_id);

  s_mqtt->registerConnectedCallback(on_mqtt_connected);
  s_mqtt->registerDataCallback(on_mqtt_data);

  // ── Start the periodic timer ─────────────────────────────────
  if (s_info_timer)
    xTimerStart(s_info_timer, 0);
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

} // namespace ED_MQTT_dispatcher