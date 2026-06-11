// ED_MQTT_dispatcher.h
#pragma once

#include "mqtt_client.h"
#include "ED_MQTT.h"
#include "ED_S_JSON.h"
#include <cstdarg>

namespace ED_MQTT_dispatcher {

// Limits
static constexpr uint8_t MAX_CMD_SUBSCRIBERS = 8;
static constexpr uint8_t MAX_JSON_PROVIDERS = 10;
static constexpr size_t CMD_ID_LEN = 32;
static constexpr size_t PARAM_KEY_LEN = 16;
static constexpr size_t PARAM_VAL_LEN = 64;
static constexpr uint8_t MAX_OPT_PARAMS = 8;

// Forward declarations
class CommandRegistry;
class GlobalCommandRegistry;
class iCommandRunner;

// ── Command parameter structure ──────────────────────────────────────
struct OptParam {
    char key[PARAM_KEY_LEN];
    char val[PARAM_VAL_LEN];
};

// ── Command descriptor ───────────────────────────────────────────────
struct ctrlCommand {
    const char* cmdID;
    const char* cmdDex;
    void (*funcPointer)(ctrlCommand*);
    uint8_t paramCount;
    OptParam optParam[MAX_OPT_PARAMS];

    const char* getParam(const char* key) const;
    bool setParam(const char* key, const char* val);
    bool addParam(const char* key, const char* default_val);
    void appendHelp(char* buf, size_t len) const;
};

// ── Command registry ─────────────────────────────────────────────────
class CommandRegistry {
public:
    static constexpr uint8_t MAX_COMMANDS = 16;

    void registerCommand(const ctrlCommand& cmd);
    ctrlCommand* getCommand(const char* cmdID) const;
    bool dispatch(const char* cmdID);
    void getHelpBrief(char* buf, size_t len) const;
    void getHelpDetail(const char* cmdID, char* buf, size_t len) const;

private:
    ctrlCommand entries[MAX_COMMANDS];
    uint8_t count = 0;
    friend class CommandWithRegistry;
};

// ── Auto‑registering wrapper ─────────────────────────────────────────
class CommandWithRegistry {
public:
    CommandWithRegistry(const char* regID, const char* briefDesc);
    void grabCommand(const char* commandID, const char* commandData,
                     size_t dataLen, uint32_t msgID);

protected:
    CommandRegistry registry;
};

// ── Global registry (singleton) ──────────────────────────────────────
struct RegistryInfo {
    const char* regID;
    CommandRegistry* registry;
    const char* briefDesc;
};

class GlobalCommandRegistry {
public:
    static GlobalCommandRegistry& instance();

    void setBaseUrl(const char* url);
    bool registerRegistry(const char* regID, CommandRegistry* reg, const char* briefDesc);
    RegistryInfo* findRegistry(const char* regID) const;
    void getHelpOverview(char* buf, size_t len) const;
    void getRegistryHelp(const char* regID, char* buf, size_t len) const;
    void getCommandHelp(const char* regID, const char* cmdID, char* buf, size_t len) const;

    uint8_t getRegistryCount() const { return m_count; }
    const char* getFirstRegistryID() const { return m_count > 0 ? m_registries[0].regID : nullptr; }

private:
    GlobalCommandRegistry() = default;
    static constexpr uint8_t MAX_REGISTRIES = 8;
    RegistryInfo m_registries[MAX_REGISTRIES];
    uint8_t m_count = 0;
    const char* m_baseUrl = nullptr;
};

// ── Command subscriber interface ────────────────────────────────────
class iCommandRunner {
public:
    virtual void grabCommand(const char* commandID, const char* commandData,
                             size_t dataLen, uint32_t msgID) = 0;
    virtual ~iCommandRunner() = default;
};

// ── Main MQTT dispatcher ─────────────────────────────────────────────
class MQTTdispatcher {
public:
    using JsonFieldProvider = void (*)(ED_S_JSON::StaticJson& doc);
    using PingSuccessCallback = void (*)(void);
    using PingFailureCallback = void (*)(void);

    static esp_err_t initialize(esp_mqtt_client_config_t* config);
    static esp_err_t run();
    static void subscribe(iCommandRunner* subscriber);
    static void registerJsonFieldProvider(JsonFieldProvider provider);
    static esp_mqtt_client_handle_t getClientHandle();

    enum class ackType { OK, FAIL };
    static void ackCommand(int64_t reqMsgID, const char* commandID,
                           ackType ackResult, const char* originalCommand = nullptr);

    static void handle_published_event(esp_mqtt_event_handle_t event);
    static void registerPingSuccessCallback(PingSuccessCallback cb);
    static void registerPingFailureCallback(PingFailureCallback cb);
    static void resetMqttReconnectAttempts();

    // Dump persistent log (RTC memory) via MQTT
    static void cmd_dumplog(ctrlCommand* cmd);

    // Persistent logging (public for dead‑man task)
    static char s_log_buffer[2048];
    static uint16_t s_log_pos;
    static void log_event(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Dead‑man monitor timestamp (public for dead‑man task)
    static int64_t s_last_good_ping_time;

private:
    static bool s_reconnect_pending;
    static void on_ip_ready();
    static void on_mqtt_connected(esp_mqtt_client_handle_t client);
    static void on_mqtt_data(esp_mqtt_client_handle_t client,
                             const char* topic, int topicLen,
                             const char* data, size_t dataLen,
                             uint32_t msgID);
    static void handleCommandObject(const char* json, size_t jsonLen, uint32_t cmdID);
    static void publishInfo();
    static void build_ping_json(char* buf, size_t len);
    static bool parseCommand(const char* input, size_t inputLen,
                             char* cmdID, size_t cmdIDLen,
                             char* payload, size_t payloadLen);
    static void T_info_timer_callback(TimerHandle_t handle);
    static void info_publisher_task(void* param);

    // Static members
    static iCommandRunner* s_subscribers[MAX_CMD_SUBSCRIBERS];
    static uint8_t s_subscriber_count;
    static esp_mqtt_client_handle_t s_clHandle;
    static TaskHandle_t s_info_task_handle;
    static TimerHandle_t s_info_timer;
    static char s_mqtt_id[18];
    static ED_MQTT::MqttClient* s_mqtt;
    static esp_mqtt_client_config_t* s_config;
    static JsonFieldProvider s_json_providers[MAX_JSON_PROVIDERS];
    static uint8_t s_json_provider_count;
    static char s_cached_ip[16];

    // Health ping state
    static int32_t s_last_ping_msg_id;
    static bool s_ping_pending;
    static uint8_t s_ping_fail_count;
    static constexpr uint8_t PING_MAX_FAILURES = 2;

    // Ping callbacks
    static PingSuccessCallback s_ping_success_cb;
    static PingFailureCallback s_ping_failure_cb;

    // Multi-level recovery counters
    static uint8_t s_mqtt_reconnect_attempts;
    static int64_t s_last_wifi_reconnect_time;
    static constexpr uint8_t MQTT_RECONNECT_THRESHOLD = 6;
    static constexpr int64_t WIFI_RECONNECT_COOLDOWN_SEC = 300; // 5 minutes
};

} // namespace ED_MQTT_dispatcher