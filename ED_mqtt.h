#pragma once
#include <esp_event_base.h>
#include <mqtt_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/task.h>

namespace ED_MQTT {

/**
 * MQTT client wrapping esp-mqtt with TLS/MQTT5 support.
 *
 * TLS notes:
 *  - Broker sends its server cert (signed by CA), NOT the CA cert.
 *  - Verification via esp_crt_bundle_attach (no per-device cert storage).
 *  - CONFIG_MQTT_PROTOCOL_5=y required in sdkconfig for MQTT5.
 *
 * Allocation policy (required for indefinite runtime):
 *  - Callbacks: fixed static arrays — no std::function, no heap.
 *  - Payload reassembly: static char buffer — no std::string resize cycles.
 *  - Singleton: raw pointer (lives forever, intentionally never freed).
 */

ESP_EVENT_DECLARE_BASE(ED_MQTT_SENSOR_EVENTS);
enum { ED_MQTT_SENSOR_EVENT_DATA_READY, ED_MQTT_SENSOR_EVENT_ERROR };

extern const char *mqtt_event_names[];

// ── Callback types — plain function pointers, zero heap ─────────────────────

/// Fired once on every successful broker connection.
using MqttConnectedCallback = void (*)(esp_mqtt_client_handle_t client);

/// Fired when a complete MQTT message has been reassembled.
/// data/dataLen point into a static buffer valid ONLY during the callback.
/// topic is NOT null-terminated — always use topicLen.
using MqttDataCallback = void (*)(esp_mqtt_client_handle_t client,
                                  const char *topic,  int    topicLen,
                                  const char *data,   size_t dataLen,
                                  int64_t     msgID);

// ── Compile-time limits ──────────────────────────────────────────────────────
static constexpr uint8_t MAX_CONNECTED_CALLBACKS = 4;
static constexpr uint8_t MAX_DATA_CALLBACKS       = 4;
/// Max reassembled payload (bytes). Larger messages are logged and dropped.
static constexpr size_t  MAX_MQTT_PAYLOAD         = 4096;

// ────────────────────────────────────────────────────────────────────────────
class MqttClient {
public:
    /// Register a callback fired on every successful broker connection.
    /// Silently ignored if MAX_CONNECTED_CALLBACKS is already reached.
    void registerConnectedCallback(MqttConnectedCallback callback);

    /// Register a callback fired on every fully reassembled incoming message.
    /// Silently ignored if MAX_DATA_CALLBACKS is already reached.
    void registerDataCallback(MqttDataCallback callback);

    /// Create the singleton (first call) or return the existing one.
    /// Pass nullptr for config to use the built-in default from secrets.h.
    static MqttClient *create(esp_mqtt_client_config_t *config = nullptr);

    /// Publish a message to a topic. Returns true on success, false on error.
/// Uses the already connected MQTT client. No dynamic allocation.
bool publish(const char* topic, const char* message, int qos = 1, bool retain = false);

    static MqttClient *getInstance() { return _instance; }
    esp_mqtt_client_handle_t getHandle();
    virtual ~MqttClient();
    static void setInstance(MqttClient* instance);

    class MqttQoS {
    public:
        enum Value : int { QOS0 = 0, QOS1 = 1, QOS2 = 2 };
    };
protected:
    static esp_mqtt_client_config_t mqttConfig;

private:
static void reconnect_task(void *arg);
static TaskHandle_t reconnect_task_handle;
static QueueHandle_t reconnect_queue;
    static int64_t mqtt5_get_epoch_property(const esp_mqtt_event_t *event);
    static void    setDefaultConfig();

    // Internal config copy — set once at boot via setDefaultConfig() or create()

    // Static callback arrays — no heap
    static MqttConnectedCallback connected_callbacks[MAX_CONNECTED_CALLBACKS];
    static uint8_t               connected_callback_count;
    static MqttDataCallback      data_callbacks[MAX_DATA_CALLBACKS];
    static uint8_t               data_callback_count;

    // Static payload reassembly buffer — replaces std::string partialPayload.
    // Eliminates the malloc/reserve/append/clear cycle on every message.
    static char   s_payload_buf[MAX_MQTT_PAYLOAD];
    static size_t s_payload_len;
    static size_t s_payload_expected;

    // Raw singleton pointer — never deleted (lives for app lifetime)
    static MqttClient *_instance;

    bool   eventsRegistered = false;
    void   destroyClient();
    bool   isShortOutage();
    void   scheduleReconnect(uint32_t delay_ms);
    static void  mqtt_reconnect_timer_cb(TimerHandle_t xTimer);
    static int     disconnect_count;
    static int64_t last_disconnect_time;

    // Teardown task: safe esp_mqtt_client_destroy() from a dedicated task context
    static void        teardown_task(void *arg); // arg = MqttClient* self
    static TaskHandle_t teardown_task_handle;

    static void mqtt_event_trampoline(void *handler_args, esp_event_base_t base,
                                      int32_t event_id, void *event_data);

protected:
    esp_err_t                start(esp_mqtt_client_config_t config);
    esp_mqtt_client_handle_t client = nullptr;
    virtual void             handleEvent(esp_event_base_t base, int32_t event_id,
                                         void *event_data);
};

// ── Sample derived class (reference template, not used in production) ────────
class SAMPLE_derivedMqttClient : public MqttClient {
private:
    bool eventsRegistered = false;
    // static SAMPLE_derivedMqttClient *_instance; // shadows base _instance

public:
    static esp_err_t create(esp_mqtt_client_config_t config);
    void send_ping_message(const char* message);
    static SAMPLE_derivedMqttClient *getInstance() { return static_cast<SAMPLE_derivedMqttClient*>(MqttClient::getInstance()); }

protected:
    void handleEvent(esp_event_base_t base, int32_t event_id,
                     void *event_data) override;

private:
    int messageCounter = 0;
};

} // namespace ED_MQTT