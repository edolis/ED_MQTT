#pragma once

#include "ED_mqtt.h"
#include "esp_timer.h"
#include "freertos/timers.h"
#include "secrets.h"

using namespace ED_MQTT;

namespace ED_MQTT_dispatcher {

// ── Compile-time limits ───────────────────────────────────────────────────────
static constexpr uint8_t MAX_COMMANDS        = 16;
static constexpr uint8_t MAX_OPT_PARAMS      = 8;
static constexpr uint8_t MAX_CMD_SUBSCRIBERS = 4;
static constexpr uint8_t MAX_DATA_LOOPS      = 4;

static constexpr uint8_t CMD_ID_LEN          = 16;
static constexpr uint8_t CMD_DEX_LEN         = 64;
static constexpr uint8_t PARAM_KEY_LEN       = 16;
static constexpr uint8_t PARAM_VAL_LEN       = 64;

// ── Command parameter: fixed key + mutable value ──────────────────────────────
struct CmdParam {
    char key[PARAM_KEY_LEN];
    char val[PARAM_VAL_LEN];
};

// ── ctrlCommand ───────────────────────────────────────────────────────────────
// Fully static: no std::string, no std::unordered_map, no std::function.
struct ctrlCommand {
    enum class cmdScope { LOCALONLY, GLOBAL };

    char     cmdID[CMD_ID_LEN]  = {};
    char     cmdDex[CMD_DEX_LEN] = {};
    cmdScope scope               = cmdScope::LOCALONLY;

    // Fixed parameter table. Keys are set at registration; values updated on
    // each invocation by the command parser. Use getParam / setParam.
    CmdParam optParam[MAX_OPT_PARAMS] = {};
    uint8_t  paramCount               = 0;

    // Plain function pointer — no std::function, no heap closure.
    void (*funcPointer)(ctrlCommand *) = nullptr;

    // ── Helpers ───────────────────────────────────────────────────────────────
    /// Returns the value for key, or nullptr if not found.
    const char *getParam(const char *key) const;

    /// Set or overwrite the value for an existing key. Returns false if the
    /// key does not exist (to add new keys, use addParam at registration time).
    bool setParam(const char *key, const char *val);

    /// Add a new key+default-value slot. Call during registration only.
    /// Returns false if the table is full (MAX_OPT_PARAMS reached).
    bool addParam(const char *key, const char *default_val = "");

    /// Append a one-line help string to buf (max len bytes).
    void appendHelp(char *buf, size_t len) const;
};

// ── CommandRegistry ───────────────────────────────────────────────────────────
// Linear-search table — MAX_COMMANDS entries, allocated at compile time.
class CommandRegistry {
public:
    void       registerCommand(const ctrlCommand &cmd);
    ctrlCommand *getCommand(const char *cmdID);
    bool         dispatch(const char *cmdID);
    void         getHelp(char *buf, size_t len) const;

private:
    ctrlCommand entries[MAX_COMMANDS] = {};
    uint8_t     count                 = 0;
};

// ── iCommandRunner ────────────────────────────────────────────────────────────
class iCommandRunner {
public:
    virtual void grabCommand(const char *commandID,
                             const char *commandData,
                             size_t      dataLen,
                             int64_t     msgID) = 0;
};

// ── CommandWithRegistry ───────────────────────────────────────────────────────
class CommandWithRegistry : public iCommandRunner {
public:
    CommandRegistry registry;

    void grabCommand(const char *commandID,
                     const char *commandData,
                     size_t      dataLen,
                     int64_t     msgID) override;

    void registerCommand(const ctrlCommand &cmd) { registry.registerCommand(cmd); }
    bool dispatchCommand(const char *cmdID)       { return registry.dispatch(cmdID); }
    void help(char *buf, size_t len) const        { registry.getHelp(buf, len); }
};

// ── MQTTdispatcher ────────────────────────────────────────────────────────────
class MQTTdispatcher {
public:
    enum ackType { OK, FAIL };

    /// Store config and register app callbacks. Does NOT start MQTT yet.
    /// Call run() after WiFi has an IP to start the MQTT connection.
    static esp_err_t initialize(esp_mqtt_client_config_t *config = nullptr);

    /// Subscribe to the WiFi IP-ready event, then start MQTT.
    /// Safe to call from app_main — actual connection happens asynchronously
    /// once the IP is available.
    static esp_err_t run();

    static void handleCommandObject(const char *json, size_t jsonLen, int64_t cmdID);

    static void ackCommand(int64_t     reqMsgID,
                           const char *commandID,
                           ackType     ackResult,
                           const char *payload);

    static void subscribe(iCommandRunner *subscriber);

private:
    // ── Internal state — all static, all fixed size ───────────────────────────
    static iCommandRunner *s_subscribers[MAX_CMD_SUBSCRIBERS];
    static uint8_t         s_subscriber_count;

    static esp_mqtt_client_handle_t  s_clHandle;
    static TaskHandle_t              s_info_task_handle;
    static TimerHandle_t             s_info_timer;
    static TimerHandle_t             s_data_loops[MAX_DATA_LOOPS];
    static uint8_t                   s_data_loop_count;

    static char s_mqtt_id[18];

    static ED_MQTT::MqttClient       *s_mqtt;
    static esp_mqtt_client_config_t  *s_config; // pointer to caller's config

    // ── MQTT callbacks (plain static functions, no lambda/std::function) ──────
    static void on_mqtt_connected(esp_mqtt_client_handle_t client);
    static void on_mqtt_data(esp_mqtt_client_handle_t client,
                             const char *topic,  int    topicLen,
                             const char *data,   size_t dataLen,
                             int64_t     msgID);

    // ── IP-ready callback (called by ED_WIFI once DHCP completes) ────────────
    static void on_ip_ready();

    // ── Command parsing ───────────────────────────────────────────────────────
    /// Parse ":CMDID [payload]" format into separate fixed-size buffers.
    /// Returns true if the input matched the command format.
    static bool parseCommand(const char *input, size_t inputLen,
                             char *cmdID,  size_t cmdIDLen,
                             char *payload, size_t payloadLen);

    // ── Periodic publish ──────────────────────────────────────────────────────
    static void build_ping_json(char *buf, size_t len);
    static void T_info_timer_callback(TimerHandle_t handle);
    static void info_publisher_task(void *arg);
    static void publishInfo();

    MQTTdispatcher() = default;
};

} // namespace ED_MQTT_dispatcher