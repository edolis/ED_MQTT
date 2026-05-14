#pragma once

#include "ED_mqtt.h"
#include "freertos/timers.h"
#include "secrets.h"
#include "ED_S_JSON.h"   // <-- ADDED: needed for JsonFieldProvider

namespace ED_MQTT_dispatcher {

// ── Compile-time limits ───────────────────────────────────────────────
static constexpr uint8_t MAX_COMMANDS        = 16;
static constexpr uint8_t MAX_OPT_PARAMS      = 8;
static constexpr uint8_t MAX_CMD_SUBSCRIBERS = 4;
static constexpr uint8_t MAX_REGISTRIES      = 8;

static constexpr uint8_t CMD_ID_LEN          = 16;
static constexpr uint8_t CMD_DEX_LEN         = 64;
static constexpr uint8_t PARAM_KEY_LEN       = 16;
static constexpr uint8_t PARAM_VAL_LEN       = 64;


// ── CmdParam ─────────────────────────────────────────────────────────
struct CmdParam {
    char key[PARAM_KEY_LEN];
    char val[PARAM_VAL_LEN];
};

// ── ctrlCommand ──────────────────────────────────────────────────────
struct ctrlCommand {
    enum class cmdScope { LOCALONLY, GLOBAL };

    char        cmdID[CMD_ID_LEN] = {};
    const char* cmdDex = nullptr;
    cmdScope    scope = cmdScope::LOCALONLY;

    CmdParam    optParam[MAX_OPT_PARAMS] = {};
    uint8_t     paramCount = 0;

    void (*funcPointer)(ctrlCommand*) = nullptr;

    const char* getParam(const char* key) const;
    bool setParam(const char* key, const char* val);
    bool addParam(const char* key, const char* default_val = "");
    void appendHelp(char* buf, size_t len) const;
};

// ── CommandRegistry ──────────────────────────────────────────────────
class CommandRegistry {
public:
    void registerCommand(const ctrlCommand& cmd);
    ctrlCommand* getCommand(const char* cmdID) const;
    bool dispatch(const char* cmdID);
    void getHelpBrief(char* buf, size_t len) const;
    void getHelpDetail(const char* cmdID, char* buf, size_t len) const;

private:
    ctrlCommand entries[MAX_COMMANDS] = {};
    uint8_t     count = 0;
};

// ── iCommandRunner ──────────────────────────────────────────────────
class iCommandRunner {
public:
    virtual void grabCommand(const char* commandID,
                             const char* commandData,
                             size_t      dataLen,
                             uint32_t     msgID) = 0;
    virtual ~iCommandRunner() = default;
};

// ── CommandWithRegistry ──────────────────────────────────────────────
class CommandWithRegistry : public iCommandRunner {
public:
    // Constructor that automatically registers this registry with GlobalCommandRegistry
    CommandWithRegistry(const char* regID, const char* briefDesc = nullptr);

    CommandRegistry registry;

    void grabCommand(const char* commandID,
                     const char* commandData,
                     size_t      dataLen,
                     uint32_t     msgID) override;

    void registerCommand(const ctrlCommand& cmd) { registry.registerCommand(cmd); }
    bool dispatchCommand(const char* cmdID)     { return registry.dispatch(cmdID); }
};

// ── RegistryInfo (for GlobalCommandRegistry) ────────────────────────
struct RegistryInfo {
    const char* regID;
    CommandRegistry* registry;
    const char* briefDesc;
};

// ── GlobalCommandRegistry (singleton) ───────────────────────────────
class GlobalCommandRegistry {
public:
    static GlobalCommandRegistry& instance();
     uint8_t getRegistryCount() const { return m_count; }
    const char* getFirstRegistryID() const {
        return (m_count > 0) ? m_registries[0].regID : nullptr;
    }

    void setBaseUrl(const char* url);
    bool registerRegistry(const char* regID, CommandRegistry* reg, const char* briefDesc = nullptr);
    void getHelpOverview(char* buf, size_t len) const;
    void getRegistryHelp(const char* regID, char* buf, size_t len) const;
    void getCommandHelp(const char* regID, const char* cmdID, char* buf, size_t len) const;

private:
    GlobalCommandRegistry() = default;
    RegistryInfo m_registries[MAX_REGISTRIES];
    uint8_t      m_count = 0;
    const char*  m_baseUrl = nullptr;

    RegistryInfo* findRegistry(const char* regID) const;
};

// ── MQTTdispatcher ──────────────────────────────────────────────────
class MQTTdispatcher {
public:

    static esp_mqtt_client_handle_t getClientHandle();
    enum ackType { OK, FAIL };

    // --- JSON field provider type and registration ---
    using JsonFieldProvider = void (*)(ED_S_JSON::StaticJson& json);
    static void registerJsonFieldProvider(JsonFieldProvider provider);

    static esp_err_t initialize(esp_mqtt_client_config_t* config = nullptr);
    static esp_err_t run();
    static void subscribe(iCommandRunner* subscriber);
    static void ackCommand(int64_t reqMsgID, const char* commandID,
                           ackType ackResult, const char* originalCommand);

    // --- Timer control (used by PFREQ) ---
    static TimerHandle_t s_info_timer;   // make accessible

private:
static char s_cached_ip[16];   // enough for IPv4

    static void on_mqtt_connected(esp_mqtt_client_handle_t client);
    static void on_mqtt_data(esp_mqtt_client_handle_t client,
                             const char* topic, int topicLen,
                             const char* data, size_t dataLen,
                             uint32_t msgID);
    static void on_ip_ready();

    static bool parseCommand(const char* input, size_t inputLen,
                             char* cmdID, size_t cmdIDLen,
                             char* payload, size_t payloadLen);

    static void build_ping_json(char* buf, size_t len);
    static void T_info_timer_callback(TimerHandle_t handle);
    static void info_publisher_task(void* arg);
    static void publishInfo();
    static void handleCommandObject(const char* json, size_t jsonLen, uint32_t cmdID);

    // --- Static members ---
    static iCommandRunner* s_subscribers[MAX_CMD_SUBSCRIBERS];
    static uint8_t         s_subscriber_count;
    static esp_mqtt_client_handle_t s_clHandle;
    static TaskHandle_t    s_info_task_handle;
    // s_info_timer is now public (declared above)
    static char            s_mqtt_id[18];
    static ED_MQTT::MqttClient*   s_mqtt;
    static esp_mqtt_client_config_t* s_config;

    // --- JSON provider storage ---
    static constexpr uint8_t MAX_JSON_PROVIDERS = 8;
    static JsonFieldProvider s_json_providers[MAX_JSON_PROVIDERS];
    static uint8_t           s_json_provider_count;
};

} // namespace ED_MQTT_dispatcher