#include "ED_MQTT_dispatcher.h"
#include "ED_json.h"
#include "ED_sys.h"
#include "ED_wifi.h"  // for subscribeToIPReady
#include <cstring>
#include <cctype>
#include <esp_log.h>

namespace ED_MQTT_dispatcher {

static const char *TAG = "MQTTdisp";

// ─────────────────────────────────────────────────────────────────────────────
//  Static member definitions
// ─────────────────────────────────────────────────────────────────────────────

iCommandRunner *MQTTdispatcher::s_subscribers[MAX_CMD_SUBSCRIBERS] = {};
uint8_t         MQTTdispatcher::s_subscriber_count                  = 0;

esp_mqtt_client_handle_t MQTTdispatcher::s_clHandle       = nullptr;
TaskHandle_t             MQTTdispatcher::s_info_task_handle = nullptr;
TimerHandle_t            MQTTdispatcher::s_info_timer       = nullptr;
TimerHandle_t            MQTTdispatcher::s_data_loops[MAX_DATA_LOOPS] = {};
uint8_t                  MQTTdispatcher::s_data_loop_count            = 0;

char                          MQTTdispatcher::s_mqtt_id[18]  = {};
ED_MQTT::MqttClient          *MQTTdispatcher::s_mqtt         = nullptr;
esp_mqtt_client_config_t     *MQTTdispatcher::s_config       = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
//  ctrlCommand helpers
// ─────────────────────────────────────────────────────────────────────────────

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
    return false; // key not found
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

void ctrlCommand::appendHelp(char *buf, size_t len) const {
    size_t used = strnlen(buf, len);
    int n = snprintf(buf + used, len - used,
                     ":%s [%s] \"%s\"\n",
                     cmdID,
                     scope == cmdScope::GLOBAL ? "GLOBAL" : "LOCAL",
                     cmdDex);
    used += (n > 0) ? (size_t)n : 0;
    for (uint8_t i = 0; i < paramCount && used < len - 1; ++i) {
        n = snprintf(buf + used, len - used,
                     "  -%s  (default: \"%s\")\n",
                     optParam[i].key, optParam[i].val);
        used += (n > 0) ? (size_t)n : 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  CommandRegistry
// ─────────────────────────────────────────────────────────────────────────────

void CommandRegistry::registerCommand(const ctrlCommand &cmd) {
    if (count >= MAX_COMMANDS) {
        ESP_LOGE("CmdReg", "Command table full (max %d)", MAX_COMMANDS);
        return;
    }
    // Avoid duplicates
    for (uint8_t i = 0; i < count; ++i)
        if (strncmp(entries[i].cmdID, cmd.cmdID, CMD_ID_LEN) == 0) {
            entries[i] = cmd; // overwrite
            return;
        }
    entries[count++] = cmd;
}

ctrlCommand *CommandRegistry::getCommand(const char *cmdID) {
    for (uint8_t i = 0; i < count; ++i)
        if (strncmp(entries[i].cmdID, cmdID, CMD_ID_LEN) == 0)
            return &entries[i];
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

void CommandRegistry::getHelp(char *buf, size_t len) const {
    if (len == 0) return;
    buf[0] = '\0';
    for (uint8_t i = 0; i < count; ++i)
        entries[i].appendHelp(buf, len);
}

// ─────────────────────────────────────────────────────────────────────────────
//  CommandWithRegistry::grabCommand
//  Parses raw payload "-flagName value …" into ctrlCommand.optParam slots,
//  then calls the command's funcPointer.
//  No std::string — all work done on const char* with pointer arithmetic.
// ─────────────────────────────────────────────────────────────────────────────

void CommandWithRegistry::grabCommand(const char *commandID,
                                      const char *commandData,
                                      size_t      /*dataLen*/,
                                      int64_t     msgID) {
    ctrlCommand *cmd = registry.getCommand(commandID);
    if (!cmd) return;

    // Store message ID
    char idbuf[24];
    snprintf(idbuf, sizeof idbuf, "%lld", (long long)msgID);
    if (!cmd->setParam("_msgID", idbuf)) cmd->addParam("_msgID", idbuf);

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
        if (n >= sizeof tmp) n = sizeof tmp - 1;
        memcpy(tmp, d0, n);
        tmp[n] = '\0';
        if (!cmd->setParam("_default", tmp)) cmd->addParam("_default", tmp);
    }

    // Remaining tokens: -key [value]
    while (*p) {
        while (*p && isspace((unsigned char)*p)) ++p;
        if (*p != '-') break;
        ++p;

        // key
        const char *f0 = p;
        while (*p && isalnum((unsigned char)*p)) ++p;
        char flagbuf[PARAM_KEY_LEN];
        size_t flen = (size_t)(p - f0);
        if (flen >= sizeof flagbuf) flen = sizeof flagbuf - 1;
        memcpy(flagbuf, f0, flen);
        flagbuf[flen] = '\0';

        // optional value
        while (*p && isspace((unsigned char)*p)) ++p;
        char valbuf[PARAM_VAL_LEN] = {};
        if (*p && *p != '-') {
            const char *v0 = p;
            while (*p && !isspace((unsigned char)*p)) ++p;
            size_t vlen = (size_t)(p - v0);
            if (vlen >= sizeof valbuf) vlen = sizeof valbuf - 1;
            memcpy(valbuf, v0, vlen);
        }

        if (!cmd->setParam(flagbuf, valbuf)) cmd->addParam(flagbuf, valbuf);
    }

    if (cmd->funcPointer) cmd->funcPointer(cmd);
}

// ─────────────────────────────────────────────────────────────────────────────
//  MQTTdispatcher — subscribe
// ─────────────────────────────────────────────────────────────────────────────

void MQTTdispatcher::subscribe(iCommandRunner *subscriber) {
    if (s_subscriber_count >= MAX_CMD_SUBSCRIBERS) {
        ESP_LOGE(TAG, "subscriber table full (max %d)", MAX_CMD_SUBSCRIBERS);
        return;
    }
    s_subscribers[s_subscriber_count++] = subscriber;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Command parsing: ":CMDID [payload]"
// ─────────────────────────────────────────────────────────────────────────────

bool MQTTdispatcher::parseCommand(const char *input, size_t inputLen,
                                  char *cmdID,  size_t cmdIDLen,
                                  char *payload, size_t payloadLen) {
    if (!input || inputLen == 0 || input[0] != ':') return false;

    size_t i = 1;
    while (i < inputLen && isspace((unsigned char)input[i])) ++i;
    if (i >= inputLen) return false;

    size_t start = i;
    while (i < inputLen && !isspace((unsigned char)input[i])) ++i;

    size_t idlen = i - start;
    if (idlen >= cmdIDLen) idlen = cmdIDLen - 1;
    for (size_t k = 0; k < idlen; ++k)
        cmdID[k] = (char)toupper((unsigned char)input[start + k]);
    cmdID[idlen] = '\0';

    while (i < inputLen && isspace((unsigned char)input[i])) ++i;

    size_t paylen = inputLen - i;
    if (paylen >= payloadLen) paylen = payloadLen - 1;
    memcpy(payload, input + i, paylen);
    payload[paylen] = '\0';

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  MQTT connected callback — plain static function, no lambda, no heap
// ─────────────────────────────────────────────────────────────────────────────

void MQTTdispatcher::on_mqtt_connected(esp_mqtt_client_handle_t client) {
    static char topic_conn[64];
    static char topic_info[64];
    static bool built = false;

    if (!built) {
        snprintf(topic_conn, sizeof topic_conn, "devices/connection/%s", s_mqtt_id);
        snprintf(topic_info, sizeof topic_info, "info/connection/%s", s_mqtt_id);
        built = true;
    }

    char msg[96];
    int n = snprintf(msg, sizeof msg, "%s connects.", s_mqtt_id);
    if (n < 0) n = 0;

    esp_mqtt_client_publish(client, topic_conn, msg, n,
                            MqttClient::MqttQoS::QOS1, true);
    esp_mqtt_client_subscribe(client, "cmd", 0);

    // Publish info JSON
    static char info_buf[512];
    build_ping_json(info_buf, sizeof info_buf);
    esp_mqtt_client_publish(client, topic_info, info_buf, strlen(info_buf),
                            MqttClient::MqttQoS::QOS1, true);

    // Update the handle in case client was recreated
    s_clHandle = client;
}

// ─────────────────────────────────────────────────────────────────────────────
//  MQTT data callback — plain static function
//  data/dataLen come from the static s_payload_buf in ED_mqtt.cpp — do NOT
//  store the pointer beyond this call, and do NOT construct std::string from it.
// ─────────────────────────────────────────────────────────────────────────────

void MQTTdispatcher::on_mqtt_data(esp_mqtt_client_handle_t /*client*/,
                                  const char */*topic*/, int /*topicLen*/,
                                  const char *data,      size_t dataLen,
                                  int64_t msgID) {
    // ── Try ":CMD payload" format ─────────────────────────────────────────────
    char cmdID[CMD_ID_LEN];
    char payload_buf[256]; // max CLI payload we accept
    if (parseCommand(data, dataLen, cmdID, sizeof cmdID, payload_buf, sizeof payload_buf)) {
        if (strcmp(cmdID, "H") == 0 || strcmp(cmdID, "HELP") == 0 ||
            strcmp(cmdID, "HLP") == 0) {
            ESP_LOGI(TAG, "HELP requested — not yet implemented");
            return;
        }
        for (uint8_t i = 0; i < s_subscriber_count; ++i)
            if (s_subscribers[i])
                s_subscribers[i]->grabCommand(cmdID, payload_buf,
                                              strlen(payload_buf), msgID);
        return;
    }

    // ── Try JSON format ───────────────────────────────────────────────────────
    // ED_JSON::JsonEncoder takes a null-terminated string. The payload buffer in
    // ED_mqtt.cpp is MAX_MQTT_PAYLOAD+1 bytes and is null-terminated after
    // assembly, so passing data directly is safe.
    handleCommandObject(data, dataLen, msgID);
}

// ─────────────────────────────────────────────────────────────────────────────
//  handleCommandObject — parse JSON and dispatch to subscribers
// ─────────────────────────────────────────────────────────────────────────────

void MQTTdispatcher::handleCommandObject(const char *json, size_t /*jsonLen*/,
                                         int64_t cmdID) {
    // ED_JSON::JsonEncoder is used as-is — we have no control over its
    // internal allocations, but command reception is infrequent so transient
    // allocation here is acceptable.
    ED_JSON::JsonEncoder decoder(json);
    if (!decoder.isValidJson()) return;

    auto unwrapped = decoder.unwrapNestedArray();
    if (unwrapped.isArray()) {
        int sz = unwrapped.getArraySize();
        for (int i = 0; i < sz; ++i) {
            auto item = unwrapped.getArrayItem(i);
            auto cmdOpt  = item.getString("cmd");
            auto dataOpt = item.getString("data");
            if (cmdOpt && dataOpt) {
                const char *c = cmdOpt->c_str();
                const char *d = dataOpt->c_str();
                for (uint8_t j = 0; j < s_subscriber_count; ++j)
                    if (s_subscribers[j])
                        s_subscribers[j]->grabCommand(c, d, strlen(d), cmdID);
            }
        }
    } else if (unwrapped.isValidJson()) {
        auto cmdOpt  = unwrapped.getString("cmd");
        auto dataOpt = unwrapped.getString("data");
        if (cmdOpt && dataOpt) {
            const char *c = cmdOpt->c_str();
            const char *d = dataOpt->c_str();
            for (uint8_t i = 0; i < s_subscriber_count; ++i)
                if (s_subscribers[i])
                    s_subscribers[i]->grabCommand(c, d, strlen(d), cmdID);
        }
    } else {
        ESP_LOGW(TAG, "Malformed payload — not a command and not valid JSON");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  ackCommand — no std::string in signature
// ─────────────────────────────────────────────────────────────────────────────

void MQTTdispatcher::ackCommand(int64_t reqMsgID, const char *commandID,
                                ackType ackResult, const char *payload) {
    static char topic_ack[64];
    static bool built = false;
    if (!built) {
        snprintf(topic_ack, sizeof topic_ack, "ack/%s", s_mqtt_id);
        built = true;
    }

    char ackbuf[256];
    int n = snprintf(ackbuf, sizeof ackbuf, "[%lld][%s] %s\n%s",
                     (long long)reqMsgID,
                     commandID ? commandID : "?",
                     ackResult == ackType::OK ? "OK" : "FAIL",
                     payload ? payload : "");
    if (n < 0) n = 0;
    if (n >= (int)sizeof ackbuf) n = (int)sizeof ackbuf - 1;

    int rc = esp_mqtt_client_publish(s_clHandle, topic_ack, ackbuf, n,
                                     MqttClient::MqttQoS::QOS1, false);
    ESP_LOGI(TAG, "ackCommand rc=%d", rc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Periodic info publish — static buffer, no std::string
// ─────────────────────────────────────────────────────────────────────────────

void MQTTdispatcher::build_ping_json(char *buf, size_t len) {
    // Build JSON manually — avoids ED_JSON heap alloc on the hot periodic path
    int n = snprintf(buf, len,
        "{\"StationID\":\"%s\",\"NetID\":\"%s\",\"MAC\":\"%s\","
        "\"IP\":\"%s\",\"time\":\"%s\",\"upTime\":\"%s\","
        "\"location\":\"\",\"Project\":\"\"}",
        ED_SYS::ESP_std::Device::mqttName(),
        ED_SYS::ESP_std::Device::netwName(),
        ED_SYS::ESP_std::Device::stdMAC(),
        ED_SYS::ESP_std::Device::curIP(),
        ED_SYS::ESP_std::Runtime::curStdTime(),
        ED_SYS::ESP_std::Runtime::uptime());
    if (n < 0 || n >= (int)len)
        buf[len - 1] = '\0';
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
    if (!s_clHandle) return;

    static char topic_info[64];
    static bool built = false;
    if (!built) {
        snprintf(topic_info, sizeof topic_info, "info/connection/%s", s_mqtt_id);
        built = true;
    }

    static char buf[512];
    build_ping_json(buf, sizeof buf);

    int rc = esp_mqtt_client_publish(s_clHandle, topic_info, buf, strlen(buf),
                                     MqttClient::MqttQoS::QOS0, true);
    if (rc < 0)
        ESP_LOGE(TAG, "publishInfo failed");
    else
        ESP_LOGI(TAG, "publishInfo ok msg_id=%d", rc);
}

// ─────────────────────────────────────────────────────────────────────────────
//  initialize() — store config, do NOT start MQTT yet (no IP yet)
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t MQTTdispatcher::initialize(esp_mqtt_client_config_t *config) {
    strncpy(s_mqtt_id, ED_SYS::ESP_std::Device::mqttName(), sizeof s_mqtt_id - 1);

    s_config = config; // store pointer — create() copies it internally

    // Register the info publish timer (not started yet — run() starts it)
    s_info_timer = xTimerCreate("info_loop",
                                pdMS_TO_TICKS(3000),
                                pdTRUE, nullptr,
                                T_info_timer_callback);
    if (!s_info_timer) {
        ESP_LOGE(TAG, "xTimerCreate failed");
        return ESP_FAIL;
    }

    xTaskCreate(info_publisher_task, "info_pub", 4096, nullptr, 5,
                &s_info_task_handle);

    ESP_LOGI(TAG, "initialized, waiting for IP before starting MQTT");
    return ESP_OK;
}

// ─────────────────────────────────────────────────────────────────────────────
//  run() — subscribe to IP-ready, then start MQTT
//  FIX: original code called resolve_uri_with_fallback() inside start() before
//  IP was available — getaddrinfo() returned immediately with EAI_AGAIN and the
//  URI fell back to the original string.  By deferring create() to on_ip_ready()
//  we guarantee the network stack is ready before the hostname lookup.
// ─────────────────────────────────────────────────────────────────────────────

esp_err_t MQTTdispatcher::run() {
    // ED_wifi::WiFiService::subscribeToIPReady takes void(*)(void)
    // (signature changed per session notes from std::function to plain pointer)
    ED_wifi::WiFiService::subscribeToIPReady(on_ip_ready);
    ESP_LOGI(TAG, "run() — MQTT will start once IP is ready");
    return ESP_OK;
}

void MQTTdispatcher::on_ip_ready() {
    ESP_LOGI(TAG, "IP ready — creating MQTT client");
    s_mqtt = MqttClient::create(s_config);
    if (!s_mqtt) {
        ESP_LOGE(TAG, "MqttClient::create failed");
        return;
    }
    s_clHandle = s_mqtt->getHandle();

    // Register dispatcher's own callbacks
    s_mqtt->registerConnectedCallback(on_mqtt_connected);
    s_mqtt->registerDataCallback(on_mqtt_data);

    // Start the periodic info loop
    if (s_info_timer)
        xTimerStart(s_info_timer, 0);
}

} // namespace ED_MQTT_dispatcher