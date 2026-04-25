#include "ED_mqtt.h"
#include "heap_tracer.h"
#include "ED_sys.h"
#include "esp_crt_bundle.h"
#include "esp_event_base.h"
#include "lwip/netdb.h"
#include "secrets.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#ifdef CONFIG_MQTT_PROTOCOL_5
#include "mqtt5_client.h"
#endif

namespace ED_MQTT {

static const char *TAG = "ED_MQTT";

// ── Static member definitions ─────────────────────────────────────────────────

// FIX: raw pointer, not unique_ptr — singleton never freed, unique_ptr destructor
// at program exit can cause issues with FreeRTOS still running.
MqttClient *MqttClient::_instance = nullptr;

esp_mqtt_client_config_t MqttClient::mqttConfig = {};

// Callback tables
MqttConnectedCallback MqttClient::connected_callbacks[MAX_CONNECTED_CALLBACKS] = {};
uint8_t               MqttClient::connected_callback_count = 0;
MqttDataCallback      MqttClient::data_callbacks[MAX_DATA_CALLBACKS] = {};
uint8_t               MqttClient::data_callback_count = 0;

// FIX: static payload buffer replaces std::string partialPayload.
// std::string does malloc+reserve+append+clear on every message — over hours
// this fragments the heap until the largest free block is too small for a new
// MQTT session and the device reboots. Static buffer: zero runtime allocation.
char   MqttClient::s_payload_buf[MAX_MQTT_PAYLOAD] = {};
size_t MqttClient::s_payload_len      = 0;
size_t MqttClient::s_payload_expected = 0;

TaskHandle_t MqttClient::teardown_task_handle = nullptr;
int          MqttClient::disconnect_count      = 0;
int64_t      MqttClient::last_disconnect_time  = 0;

static TimerHandle_t mqtt_reconnect_timer = nullptr;

// ── Event name table ──────────────────────────────────────────────────────────
const char *mqtt_event_names[] = {
    "MQTT_EVENT_ERROR",
    "MQTT_EVENT_CONNECTED",
    "MQTT_EVENT_DISCONNECTED",
    "MQTT_EVENT_SUBSCRIBED",
    "MQTT_EVENT_UNSUBSCRIBED",
    "MQTT_EVENT_PUBLISHED",
    "MQTT_EVENT_DATA",
    "MQTT_EVENT_BEFORE_CONNECT",
    "MQTT_EVENT_DELETED",
    "MQTT_USER_EVENT",
};

ESP_EVENT_DEFINE_BASE(ED_MQTT_SENSOR_EVENTS);

// ── Registration ──────────────────────────────────────────────────────────────

void MqttClient::registerConnectedCallback(MqttConnectedCallback callback) {
    if (connected_callback_count < MAX_CONNECTED_CALLBACKS) {
        connected_callbacks[connected_callback_count++] = callback;
    } else {
        ESP_LOGE(TAG, "registerConnectedCallback: table full (max %d)",
                 MAX_CONNECTED_CALLBACKS);
    }
}

void MqttClient::registerDataCallback(MqttDataCallback callback) {
    if (data_callback_count < MAX_DATA_CALLBACKS) {
        data_callbacks[data_callback_count++] = callback;
    } else {
        ESP_LOGE(TAG, "registerDataCallback: table full (max %d)",
                 MAX_DATA_CALLBACKS);
    }
}

// ── URI resolution ────────────────────────────────────────────────────────────
// IMPORTANT: call this only after WiFi has an IP address (subscribe via
// ED_wifi::WiFiService::subscribeToIPReady). Calling before IP is ready will
// cause getaddrinfo() to block/fail and uri will be NULL → client_init crash.

static char s_final_uri[128];

static const char *resolve_uri_with_fallback(const char *uri_in) {
    char scheme[8], host[64];
    int  port = 0;

    if (sscanf(uri_in, "%7[^:]://%63[^:]:%d", scheme, host, &port) < 2) {
        ESP_LOGE("ED_MQTT", "Invalid URI format: %s", uri_in);
        return uri_in; // return original rather than NULL to avoid crash
    }

    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, NULL, &res) == 0 && res) {
        freeaddrinfo(res);
        snprintf(s_final_uri, sizeof s_final_uri, "%s://%s:%d", scheme, host, port);
        ESP_LOGI("ED_MQTT", "Resolved URI: %s", s_final_uri);
        return s_final_uri;
    }

    ESP_LOGW("ED_MQTT", "Host '%s' not resolved, trying '%s.local'", host, host);
    char host_local[70];
    snprintf(host_local, sizeof host_local, "%s.local", host);

    if (getaddrinfo(host_local, NULL, NULL, &res) == 0 && res) {
        freeaddrinfo(res);
        snprintf(s_final_uri, sizeof s_final_uri, "%s://%s:%d", scheme, host_local, port);
        ESP_LOGI("ED_MQTT", "Resolved URI (mDNS): %s", s_final_uri);
        return s_final_uri;
    }

    ESP_LOGE("ED_MQTT", "Both '%s' and '%s.local' failed — using original URI", host, host);
    return uri_in; // fall back to original, let MQTT retry
}

// ── Default config ────────────────────────────────────────────────────────────
void MqttClient::setDefaultConfig() {
    static char topicBuf[64];
    static char msgBuf[64];
    snprintf(topicBuf, sizeof topicBuf, "devices/connection/%s",
             ED_SYS::ESP_std::Device::mqttName());
    snprintf(msgBuf, sizeof msgBuf, "%s disconnected unexpectedly.",
             ED_SYS::ESP_std::Device::mqttName());

    mqttConfig = {};
    mqttConfig.broker.address.uri = "mqtts://192.168.1.220:8883";
    mqttConfig.broker.verification.use_global_ca_store  = false;
    mqttConfig.broker.verification.crt_bundle_attach    = esp_crt_bundle_attach;
    mqttConfig.broker.verification.skip_cert_common_name_check = false;
    mqttConfig.credentials.username                     = ED_MQTT_USERNAME;
    mqttConfig.credentials.client_id                    = ED_SYS::ESP_std::Device::mqttName();
    mqttConfig.credentials.authentication.password      = ED_MQTT_PASSWORD;
    mqttConfig.credentials.authentication.use_secure_element = false;
    mqttConfig.session.last_will.topic                  = topicBuf;
    mqttConfig.session.last_will.msg                    = msgBuf;
    mqttConfig.session.last_will.qos                    = MqttQoS::QOS1;
    mqttConfig.session.last_will.retain                 = true;
    mqttConfig.session.protocol_ver                     = MQTT_PROTOCOL_V_5;
}

// ── Reconnect timer ───────────────────────────────────────────────────────────
void MqttClient::mqtt_reconnect_timer_cb(TimerHandle_t xTimer) {
    ESP_LOGI(TAG, "Reconnect timer fired — restarting MQTT client");
    // FIX: guard getInstance() — if teardown ran and instance is gone, do nothing
    MqttClient *self = getInstance();
    if (self == nullptr) {
        ESP_LOGE(TAG, "mqtt_reconnect_timer_cb: instance is null, cannot restart");
        return;
    }
    self->start(mqttConfig);
}

// ── Teardown task ─────────────────────────────────────────────────────────────
// FIX: original code passed NULL as task arg then cast it to MqttClient*,
// causing an immediate null-pointer dereference inside the task.
// We pass `this` explicitly and use getInstance() as a fallback.
void MqttClient::teardown_task(void *arg) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // arg is the MqttClient* that created the task — prefer it over getInstance()
        // because getInstance() could be null during re-init sequences.
        MqttClient *self = (arg != nullptr)
                               ? static_cast<MqttClient *>(arg)
                               : getInstance();
        if (self == nullptr) {
            ESP_LOGW(TAG, "teardown_task: no instance to tear down");
            continue;
        }
        if (self->client) {
            ESP_LOGW(TAG, "Destroying MQTT client from safe task context");
            esp_mqtt_client_stop(self->client);
            esp_mqtt_client_destroy(self->client);
            self->client = nullptr;
        }
    }
}

// ── Singleton creation ────────────────────────────────────────────────────────
MqttClient *MqttClient::create(esp_mqtt_client_config_t *config) {
    if (_instance != nullptr)
        return _instance; // already created

    if (config != nullptr)
        mqttConfig = *config;
    else
        setDefaultConfig();

    // Allocate instance once — this is the only heap allocation in this file
    _instance = new MqttClient();
    if (_instance == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate MqttClient instance");
        return nullptr;
    }

    // FIX: pass `_instance` as task arg so teardown_task can dereference it
    // safely. The original code passed NULL → instant crash inside the task.
    xTaskCreate(teardown_task, "mqtt_teardown", 4096,
                static_cast<void *>(_instance),
                tskIDLE_PRIORITY + 1, &teardown_task_handle);

    esp_err_t err = _instance->start(mqttConfig);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        delete _instance;
        _instance = nullptr;
        return nullptr;
    }

    return _instance;
}

esp_mqtt_client_handle_t MqttClient::getHandle() { return client; }

// ── Event trampoline ──────────────────────────────────────────────────────────
void MqttClient::mqtt_event_trampoline(void *handler_args, esp_event_base_t base,
                                       int32_t event_id, void *event_data) {
    auto *self = static_cast<MqttClient *>(handler_args);
    self->handleEvent(base, event_id, event_data);
}

// ── Start ─────────────────────────────────────────────────────────────────────
esp_err_t MqttClient::start(esp_mqtt_client_config_t config) {
    esp_err_t err;
    if (client == nullptr) {
        // Resolve hostname only if IP is available — caller must ensure this
        const char *uri = resolve_uri_with_fallback(config.broker.address.uri);
        config.broker.address.uri = uri;

        ESP_LOGI(TAG, "Heap before mqtt_init: free=%u largest=%u",
                 heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        ED_heap_tracer::mqtt_heap_trace_start();

        client = esp_mqtt_client_init(&config);
        if (client == nullptr)
            return ESP_FAIL;

        err = esp_mqtt_client_start(client);
        if (err != ESP_OK)
            return err;
    }

    if (!eventsRegistered) {
        err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                             mqtt_event_trampoline, this);
        if (err != ESP_OK)
            return err;
        eventsRegistered = true;
    }
    return ESP_OK;
}

// ── Reconnect helpers ─────────────────────────────────────────────────────────
void MqttClient::scheduleReconnect(uint32_t delay_ms) {
    if (mqtt_reconnect_timer == nullptr) {
        mqtt_reconnect_timer = xTimerCreate("mqtt_reconnect",
                                            pdMS_TO_TICKS(delay_ms),
                                            pdFALSE, nullptr,
                                            mqtt_reconnect_timer_cb);
    } else {
        xTimerStop(mqtt_reconnect_timer, 0);
        xTimerChangePeriod(mqtt_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
    }
    xTimerStart(mqtt_reconnect_timer, 0);
}

bool MqttClient::isShortOutage() {
    int64_t now        = esp_timer_get_time() / 1000000LL;
    int64_t since_last = now - last_disconnect_time;
    last_disconnect_time = now;
    disconnect_count++;

    constexpr int MAX_DISCONNECTS  = 3;
    constexpr int SHORT_WINDOW_SEC = 60;

    if (since_last > SHORT_WINDOW_SEC) {
        disconnect_count = 1;
        return true;
    }
    return (disconnect_count <= MAX_DISCONNECTS);
}

// ── Event handler ─────────────────────────────────────────────────────────────
void MqttClient::handleEvent(esp_event_base_t base, int32_t event_id,
                             void *event_data) {
    auto *event = static_cast<esp_mqtt_event_t *>(event_data);

    switch (event_id) {

    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(client, "devices/connection", 0);
#ifdef DEBUG_BUILD
        esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32",
                                0, MqttQoS::QOS1, 0);
#endif
        for (uint8_t i = 0; i < connected_callback_count; ++i) {
            if (connected_callbacks[i])
                connected_callbacks[i](event->client);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        if (isShortOutage()) {
            ESP_LOGW(TAG, "Transient disconnect — letting MQTT auto-reconnect");
        } else {
            ESP_LOGE(TAG, "Prolonged disconnect — tearing down and rebuilding client");
            ED_heap_tracer::mqtt_heap_trace_snapshot_async();
            xTaskNotify(teardown_task_handle, 0, eNoAction);
            scheduleReconnect(3000);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle) {
            ESP_LOGE(TAG, "  error_type=%d esp_tls_err=0x%x",
                     event->error_handle->error_type,
                     event->error_handle->esp_tls_last_esp_err);
        }
        // Only tear down on transport-level errors, not protocol errors.
        // MQTT_ERROR_TYPE_TCP_TRANSPORT = 1
        if (event->error_handle &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ED_heap_tracer::mqtt_heap_trace_snapshot_async();
            xTaskNotify(teardown_task_handle, 0, eNoAction);
            scheduleReconnect(5000);
        }
        break;

    case MQTT_EVENT_DATA: {
        // FIX: replaced std::string partialPayload (heap) with static char buffer.
        // The old code did: partialPayload.reserve() + .append() + .clear() per message.
        // Over thousands of messages this left the heap fragmented. Static buffer:
        // one allocation at boot, reused forever.

        if (event->current_data_offset == 0) {
            // First (or only) chunk — reset assembly state
            s_payload_len      = 0;
            s_payload_expected = event->total_data_len;
        }

        size_t incoming = (size_t)event->data_len;
        if (s_payload_len + incoming > MAX_MQTT_PAYLOAD) {
            ESP_LOGW(TAG, "Payload exceeds MAX_MQTT_PAYLOAD (%u), dropping",
                     (unsigned)MAX_MQTT_PAYLOAD);
            s_payload_len      = 0;
            s_payload_expected = 0;
            break;
        }

        memcpy(s_payload_buf + s_payload_len, event->data, incoming);
        s_payload_len += incoming;

        // Last chunk?
        bool complete = (s_payload_expected > 0)
            ? (s_payload_len >= s_payload_expected)
            : (event->current_data_offset + incoming >= (size_t)event->total_data_len);

        if (complete) {
            int64_t msgID = mqtt5_get_epoch_property(event);
            for (uint8_t i = 0; i < data_callback_count; ++i) {
                if (data_callbacks[i])
                    data_callbacks[i](event->client,
                                     event->topic, event->topic_len,
                                     s_payload_buf, s_payload_len,
                                     msgID);
            }
            s_payload_len      = 0;
            s_payload_expected = 0;
        }
        break;
    }

    default: {
        int         id   = event->event_id;
        const char *name = (id >= 0 &&
                            id < (int)(sizeof(mqtt_event_names) /
                                       sizeof(mqtt_event_names[0])))
                               ? mqtt_event_names[id]
                               : "MQTT_EVENT_UNKNOWN";
        ESP_LOGI(TAG, "MQTT event id:%d (%s)", id, name);
        break;
    }
    }
}

// ── MQTT5 epoch property ──────────────────────────────────────────────────────
int64_t MqttClient::mqtt5_get_epoch_property(const esp_mqtt_event_t *event) {
#ifdef CONFIG_MQTT_PROTOCOL_5
    if (!event || !event->property || !event->property->user_property)
        return -1;

    mqtt5_user_property_handle_t handle = event->property->user_property;
    uint8_t count = esp_mqtt5_client_get_user_property_count(handle);
    if (count == 0)
        return -1;

    // Fast path: up to 4 properties on the stack (covers 99% of cases)
    constexpr uint8_t STACK_THRESHOLD = 4;
    if (count <= STACK_THRESHOLD) {
        esp_mqtt5_user_property_item_t items[STACK_THRESHOLD];
        uint8_t actual = count;
        if (esp_mqtt5_client_get_user_property(handle, items, &actual) == ESP_OK) {
            for (uint8_t i = 0; i < actual; ++i) {
                if (items[i].key && strcmp(items[i].key, "epoch") == 0)
                    return atoll(items[i].value);
            }
        }
        return -1;
    }

    // Slow path: >4 properties, heap-allocate (rare — broker-controlled)
    auto *items = static_cast<esp_mqtt5_user_property_item_t *>(
        malloc(sizeof(esp_mqtt5_user_property_item_t) * count));
    if (!items)
        return -1;
    int64_t result = -1;
    uint8_t actual = count;
    if (esp_mqtt5_client_get_user_property(handle, items, &actual) == ESP_OK) {
        for (uint8_t i = 0; i < actual; ++i) {
            if (items[i].key && strcmp(items[i].key, "epoch") == 0) {
                result = atoll(items[i].value);
                break;
            }
        }
    }
    free(items);
    return result;
#else
    return -1;
#endif
}

// ── Destructor / destroyClient ────────────────────────────────────────────────
MqttClient::~MqttClient() {
    if (eventsRegistered && client) {
        esp_mqtt_client_unregister_event(client, MQTT_EVENT_ANY,
                                         mqtt_event_trampoline);
        eventsRegistered = false;
    }
    if (client) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = nullptr;
    }
}

void MqttClient::destroyClient() {
    if (client) {
        ESP_LOGW(TAG, "destroyClient()");
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = nullptr;
    }
}

// ── SAMPLE derived class ──────────────────────────────────────────────────────
SAMPLE_derivedMqttClient *SAMPLE_derivedMqttClient::_instance = nullptr;

esp_err_t SAMPLE_derivedMqttClient::create(esp_mqtt_client_config_t config) {
    auto *inst = new SAMPLE_derivedMqttClient();
    esp_err_t err = inst->start(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SAMPLE_derivedMqttClient::create failed: %s",
                 esp_err_to_name(err));
        delete inst;
        return err;
    }
    _instance = inst;
    return ESP_OK;
}

void SAMPLE_derivedMqttClient::handleEvent(esp_event_base_t base,
                                           int32_t event_id, void *event_data) {
    auto *event = static_cast<esp_mqtt_event_t *>(event_data);
    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "SAMPLE: MQTT_EVENT_CONNECTED");
        esp_mqtt_client_subscribe(client, "/test/topic", 0);
        break;
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "SAMPLE: DATA topic=%.*s", event->topic_len, event->topic);
        break;
    default:
        MqttClient::handleEvent(base, event_id, event_data);
        break;
    }
}

void SAMPLE_derivedMqttClient::send_ping_message() {
    char msg[64];
    snprintf(msg, sizeof msg, "%s:%lld", TAG, esp_timer_get_time() / 1000LL);
    esp_mqtt_client_publish(client, "pingESPdevice", msg, 0, 1, 0);
    ESP_LOGI(TAG, "Ping sent: %s", msg);
}

} // namespace ED_MQTT