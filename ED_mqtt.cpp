#include "ED_mqtt.h"
#include "ED_sys.h"
#include "esp_crt_bundle.h"
#include "esp_event_base.h"
#include "heap_tracer.h"
#include "lwip/netdb.h"
#include "secrets.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#ifdef CONFIG_MQTT_PROTOCOL_5
#include "mqtt5_client.h"
#endif

namespace ED_MQTT {

static const char *TAG = "ED_MQTT";

// ── Static mutex for thread safety (no dynamic allocation) ────────────────
static StaticSemaphore_t s_mqtt_mutex_buffer;
static SemaphoreHandle_t s_mqtt_mutex = nullptr;

static SemaphoreHandle_t get_mqtt_mutex() {
    if (s_mqtt_mutex == nullptr) {
      ESP_LOGI(TAG, "Creating MQTT mutex (first use)");
        s_mqtt_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_mqtt_mutex_buffer);
        configASSERT(s_mqtt_mutex);
    }
    return s_mqtt_mutex;
}

// ── Static member definitions
// ─────────────────────────────────────────────────

// Static timer for reconnection (was previously a file‑static variable)
TimerHandle_t MqttClient::mqtt_reconnect_timer = nullptr;

// Health monitor static members
TimerHandle_t MqttClient::s_health_timer = nullptr;
uint8_t MqttClient::s_publish_fail_count = 0;
MqttClient::ReconnectCallback MqttClient::s_reconnect_callback = nullptr;


MqttClient *MqttClient::_instance = nullptr;
esp_mqtt_client_config_t MqttClient::mqttConfig = {};

// Callback tables
MqttConnectedCallback MqttClient::connected_callbacks[MAX_CONNECTED_CALLBACKS] = {};
uint8_t MqttClient::connected_callback_count = 0;
MqttDataCallback MqttClient::data_callbacks[MAX_DATA_CALLBACKS] = {};
uint8_t MqttClient::data_callback_count = 0;

// Static payload buffer
char MqttClient::s_payload_buf[MAX_MQTT_PAYLOAD] = {};
size_t MqttClient::s_payload_len = 0;
size_t MqttClient::s_payload_expected = 0;

TaskHandle_t MqttClient::teardown_task_handle = nullptr;
int MqttClient::disconnect_count = 0;
int64_t MqttClient::last_disconnect_time = 0;

static TimerHandle_t mqtt_reconnect_timer = nullptr;
TaskHandle_t MqttClient::reconnect_task_handle = nullptr;
QueueHandle_t MqttClient::reconnect_queue = nullptr;

void MqttClient::reconnect_task(void *arg) {
  while (true) {
    uint32_t dummy;
    if (xQueueReceive(reconnect_queue, &dummy, portMAX_DELAY) == pdTRUE) {
      ESP_LOGI(TAG, "Reconnect task: received request, starting MQTT client");

      xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
      MqttClient *self = getInstance();
      if (self) {
        if (self->client) {
          ESP_LOGI(TAG, "Reconnect task: destroying existing client");
          self->destroyClient();
        }
        esp_err_t err = self->start(mqttConfig);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Reconnect task: start failed: %s", esp_err_to_name(err));
        } else {
          ESP_LOGI(TAG, "Reconnect task: MQTT started successfully");
        }
      } else {
        ESP_LOGE(TAG, "Reconnect task: no MQTT instance");
      }
      xSemaphoreGiveRecursive(get_mqtt_mutex());
    }
  }
}

// ── Event name table
// ──────────────────────────────────────────────────────────
const char *mqtt_event_names[] = {
    "MQTT_EVENT_ERROR",        "MQTT_EVENT_CONNECTED",
    "MQTT_EVENT_DISCONNECTED", "MQTT_EVENT_SUBSCRIBED",
    "MQTT_EVENT_UNSUBSCRIBED", "MQTT_EVENT_PUBLISHED",
    "MQTT_EVENT_DATA",         "MQTT_EVENT_BEFORE_CONNECT",
    "MQTT_EVENT_DELETED",      "MQTT_USER_EVENT",
};

ESP_EVENT_DEFINE_BASE(ED_MQTT_SENSOR_EVENTS);

// ── Registration (thread‑safe)
// ──────────────────────────────────────────────────────────────
void MqttClient::registerConnectedCallback(MqttConnectedCallback callback) {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
  if (connected_callback_count < MAX_CONNECTED_CALLBACKS) {
    connected_callbacks[connected_callback_count++] = callback;
  } else {
    ESP_LOGE(TAG, "registerConnectedCallback: table full (max %d)", MAX_CONNECTED_CALLBACKS);
  }
  xSemaphoreGiveRecursive(get_mqtt_mutex());
}

void MqttClient::registerDataCallback(MqttDataCallback callback) {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
  if (data_callback_count < MAX_DATA_CALLBACKS) {
    data_callbacks[data_callback_count++] = callback;
  } else {
    ESP_LOGE(TAG, "registerDataCallback: table full (max %d)", MAX_DATA_CALLBACKS);
  }
  xSemaphoreGiveRecursive(get_mqtt_mutex());
}

// ── URI resolution (no shared state, no mutex needed)
// ────────────────────────────────────────────────────────────
static char s_final_uri[128];

static const char *resolve_uri_with_fallback(const char *uri_in) {
  char scheme[8], host[64];
  int port = 0;

  if (sscanf(uri_in, "%7[^:]://%63[^:]:%d", scheme, host, &port) < 2) {
    ESP_LOGE("ED_MQTT", "Invalid URI format: %s", uri_in);
    return uri_in;
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
  return uri_in;
}

// ── Default config
// ────────────────────────────────────────────────────────────
void MqttClient::setDefaultConfig() {
  static char topicBuf[64];
  static char msgBuf[64];
  snprintf(topicBuf, sizeof topicBuf, "devices/connection/%s", ED_SYS::ESP_std::Device::mqttName());
  snprintf(msgBuf, sizeof msgBuf, "%s disconnected unexpectedly.", ED_SYS::ESP_std::Device::mqttName());

  mqttConfig = {};
  mqttConfig.broker.address.uri = "mqtts://192.168.1.220:8883";
  mqttConfig.broker.verification.use_global_ca_store = false;
  mqttConfig.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  mqttConfig.broker.verification.skip_cert_common_name_check = false;
  mqttConfig.credentials.username = ED_MQTT_USERNAME;
  mqttConfig.credentials.client_id = ED_SYS::ESP_std::Device::mqttName();
  mqttConfig.credentials.authentication.password = ED_MQTT_PASSWORD;
  mqttConfig.credentials.authentication.use_secure_element = false;
  mqttConfig.session.last_will.topic = topicBuf;
  mqttConfig.session.last_will.msg = msgBuf;
  mqttConfig.session.last_will.qos = MqttQoS::QOS1;
  mqttConfig.session.last_will.retain = true;
  mqttConfig.session.protocol_ver = MQTT_PROTOCOL_V_5;
}

// ── Reconnect timer callback
// ───────────────────────────────────────────────────────────
void MqttClient::mqtt_reconnect_timer_cb(TimerHandle_t xTimer) {
  ESP_LOGI(TAG, "Reconnect timer fired – sending request to reconnect task");
  if (reconnect_queue) {
    uint32_t signal = 1;
    xQueueSend(reconnect_queue, &signal, 0);
  } else {
    ESP_LOGE(TAG, "Reconnect queue not ready, cannot request reconnect");
  }
}

// ── Teardown task
// ─────────────────────────────────────────────────────────────
void MqttClient::teardown_task(void *arg) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
    MqttClient *self = (arg != nullptr) ? static_cast<MqttClient *>(arg) : getInstance();
    if (self == nullptr) {
      ESP_LOGW(TAG, "teardown_task: no instance to tear down");
      xSemaphoreGiveRecursive(get_mqtt_mutex());
      continue;
    }
    if (self->client) {
      ESP_LOGW(TAG, "Destroying MQTT client from safe task context");
      self->destroyClient();
    }
    xSemaphoreGiveRecursive(get_mqtt_mutex());
  }
}

// ── Singleton creation
// ────────────────────────────────────────────────────────
MqttClient *MqttClient::create(esp_mqtt_client_config_t *config) {
  if (_instance != nullptr)
    return _instance;

  // Mutex already exists from global initializer
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);

  if (config != nullptr)
    mqttConfig = *config;
  else
    setDefaultConfig();

  _instance = new MqttClient();
  if (_instance == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate MqttClient instance");
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    return nullptr;
  }

  xTaskCreate(teardown_task, "mqtt_teardown", 4096,
              static_cast<void *>(_instance), tskIDLE_PRIORITY + 1,
              &teardown_task_handle);

  esp_err_t err = _instance->start(mqttConfig);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    if (teardown_task_handle) {
      vTaskDelete(teardown_task_handle);
      teardown_task_handle = nullptr;
    }
    delete _instance;
    _instance = nullptr;
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    return nullptr;
  }
  setInstance(_instance);
  xSemaphoreGiveRecursive(get_mqtt_mutex());
  return _instance;
}

esp_mqtt_client_handle_t MqttClient::getHandle() {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
  auto h = client;
  xSemaphoreGiveRecursive(get_mqtt_mutex());
  return h;
}

// ── Event trampoline
// ──────────────────────────────────────────────────────────
void MqttClient::mqtt_event_trampoline(void *handler_args,
                                       esp_event_base_t base, int32_t event_id,
                                       void *event_data) {
  auto *self = static_cast<MqttClient *>(handler_args);
  self->handleEvent(base, event_id, event_data);
}

// ── Start (protected)
// ─────────────────────────────────────────────────────────────────────
esp_err_t MqttClient::start(esp_mqtt_client_config_t config) {
  ESP_LOGI(TAG, "MqttClient::start entered");
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);

  if (client != nullptr) {
    ESP_LOGW(TAG, "client already exists");
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    return ESP_OK;
  }

  const char *uri = resolve_uri_with_fallback(config.broker.address.uri);
  config.broker.address.uri = uri;
  client = esp_mqtt_client_init(&config);
  if (client == nullptr) {
    ESP_LOGE(TAG, "esp_mqtt_client_init failed");
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "MQTT client init OK");
  if (!eventsRegistered) {
    esp_err_t err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                                   mqtt_event_trampoline, this);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "register_event failed: %s", esp_err_to_name(err));
      xSemaphoreGiveRecursive(get_mqtt_mutex());
      return err;
    }
    eventsRegistered = true;
    ESP_LOGI(TAG, "Events registered");
  }
  esp_err_t err = esp_mqtt_client_start(client);
  ESP_LOGI(TAG, "esp_mqtt_client_start returned: %s", esp_err_to_name(err));
  xSemaphoreGiveRecursive(get_mqtt_mutex());
  return err;
}

// ── Reconnect helpers
// ─────────────────────────────────────────────────────────
void MqttClient::scheduleReconnect(uint32_t delay_ms) {
  if (mqtt_reconnect_timer == nullptr) {
    mqtt_reconnect_timer = xTimerCreate("mqtt_reconnect", pdMS_TO_TICKS(delay_ms), pdFALSE,
                                        nullptr, mqtt_reconnect_timer_cb);
  } else {
    xTimerStop(mqtt_reconnect_timer, 0);
    xTimerChangePeriod(mqtt_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
  }
  xTimerStart(mqtt_reconnect_timer, 0);
}

void MqttClient::health_timer_cb(TimerHandle_t xTimer) {
    if (s_publish_fail_count >= MAX_CONSECUTIVE_FAILURES) {
        ESP_LOGW(TAG, "%u consecutive publish failures, forcing reconnect",
                 s_publish_fail_count);
        forceReconnect();
        s_publish_fail_count = 0;
        if (s_reconnect_callback) {
            s_reconnect_callback();
        }
    }
}

bool MqttClient::isShortOutage() {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
  int64_t now = esp_timer_get_time() / 1000000LL;
  int64_t since_last = now - last_disconnect_time;
  last_disconnect_time = now;
  disconnect_count++;

  constexpr int MAX_DISCONNECTS = 3;
  constexpr int SHORT_WINDOW_SEC = 60;

  if (since_last > SHORT_WINDOW_SEC) {
    disconnect_count = 1;
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    return true;
  }
  bool result = (disconnect_count <= MAX_DISCONNECTS);
  xSemaphoreGiveRecursive(get_mqtt_mutex());
  return result;
}

// ── Event handler (thread‑safe)
// ─────────────────────────────────────────────────────────────
void MqttClient::handleEvent(esp_event_base_t base, int32_t event_id,
                             void *event_data) {
  auto *event = static_cast<esp_mqtt_event_t *>(event_data);

  switch (event_id) {

  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    esp_mqtt_client_subscribe(client, "devices/connection", 0);
#ifdef DEBUG_BUILD
    esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32", 0,
                            MqttQoS::QOS1, 0);
#endif
    xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
    for (uint8_t i = 0; i < connected_callback_count; ++i) {
      if (connected_callbacks[i])
        connected_callbacks[i](event->client);
    }
    xSemaphoreGiveRecursive(get_mqtt_mutex());
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
    if (event->error_handle &&
        event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
      ED_heap_tracer::mqtt_heap_trace_snapshot_async();
      xTaskNotify(teardown_task_handle, 0, eNoAction);
      scheduleReconnect(5000);
    }
    break;

  case MQTT_EVENT_DATA: {
    xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);

    if (event->current_data_offset == 0) {
      s_payload_len = 0;
      s_payload_expected = event->total_data_len;
    }

    size_t incoming = (size_t)event->data_len;
    if (s_payload_len + incoming > MAX_MQTT_PAYLOAD) {
      ESP_LOGW(TAG, "Payload exceeds MAX_MQTT_PAYLOAD (%u), dropping",
               (unsigned)MAX_MQTT_PAYLOAD);
      s_payload_len = 0;
      s_payload_expected = 0;
      xSemaphoreGiveRecursive(get_mqtt_mutex());
      break;
    }

    memcpy(s_payload_buf + s_payload_len, event->data, incoming);
    s_payload_len += incoming;

    bool complete = (s_payload_expected > 0)
                        ? (s_payload_len >= s_payload_expected)
                        : (event->current_data_offset + incoming >=
                           (size_t)event->total_data_len);

    if (complete) {
      int64_t msgID = mqtt5_get_epoch_property(event);
      for (uint8_t i = 0; i < data_callback_count; ++i) {
        if (data_callbacks[i])
          data_callbacks[i](event->client, event->topic, event->topic_len,
                            s_payload_buf, s_payload_len, msgID);
      }
      s_payload_len = 0;
      s_payload_expected = 0;
    }
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    break;
  }

  default: {
    int id = event->event_id;
    const char *name = (id >= 0 && id < (int)(sizeof(mqtt_event_names) /
                                              sizeof(mqtt_event_names[0])))
                           ? mqtt_event_names[id]
                           : "MQTT_EVENT_UNKNOWN";
    ESP_LOGI(TAG, "MQTT event id:%d (%s)", id, name);
    break;
  }
  }
}

// ── MQTT5 epoch property
// ──────────────────────────────────────────────────────
int64_t MqttClient::mqtt5_get_epoch_property(const esp_mqtt_event_t *event) {
#ifdef CONFIG_MQTT_PROTOCOL_5
  if (!event || !event->property || !event->property->user_property)
    return -1;

  mqtt5_user_property_handle_t handle = event->property->user_property;
  uint8_t count = esp_mqtt5_client_get_user_property_count(handle);
  if (count == 0)
    return -1;

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

// ── Destructor / destroyClient (thread‑safe)
// ────────────────────────────────────────────────
MqttClient::~MqttClient() {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
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
  xSemaphoreGiveRecursive(get_mqtt_mutex());
}

void MqttClient::destroyClient() {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
  if (client) {
    ESP_LOGW(TAG, "destroyClient()");
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
    eventsRegistered = false;
  }
  xSemaphoreGiveRecursive(get_mqtt_mutex());
}

void MqttClient::setInstance(MqttClient *instance) {
  xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
  if (_instance == nullptr && instance != nullptr) {
    _instance = instance;
    if (reconnect_queue == nullptr) {
      reconnect_queue = xQueueCreate(5, sizeof(uint32_t));
      configASSERT(reconnect_queue);
    }
    if (reconnect_task_handle == nullptr) {
      xTaskCreate(reconnect_task, "mqtt_reconnect", 4096, nullptr,
                  tskIDLE_PRIORITY + 2, &reconnect_task_handle);
    }
    if (teardown_task_handle == nullptr) {
      xTaskCreate(teardown_task, "mqtt_teardown", 4096,
                  static_cast<void *>(_instance), tskIDLE_PRIORITY + 1,
                  &teardown_task_handle);
    }
  }
  if (s_health_timer == nullptr) {
    s_health_timer = xTimerCreate("mqtt_health",
                                   pdMS_TO_TICKS(30000),  // check every 30 sec
                                   pdTRUE,
                                   nullptr,
                                   health_timer_cb);
    if (s_health_timer) {
        xTimerStart(s_health_timer, 0);
        ESP_LOGI(TAG, "Health monitor started (30s interval)");
    }
}
  xSemaphoreGiveRecursive(get_mqtt_mutex());
}

bool MqttClient::publish(const char* topic, const char* message, int qos, bool retain) {
    xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
    bool ok = false;
    if (client == nullptr) {
        ESP_LOGE(TAG, "publish: MQTT client handle is null");
    } else if (topic == nullptr || message == nullptr) {
        ESP_LOGE(TAG, "publish: topic or message is null");
    } else {
        int msg_id = esp_mqtt_client_publish(client, topic, message, 0, qos, retain ? 1 : 0);
        if (msg_id < 0) {
            ESP_LOGE(TAG, "publish failed to '%s': error %d", topic, msg_id);
            s_publish_fail_count++;                     // ← failure counter
        } else {
            ESP_LOGD(TAG, "Published to '%s': %s (msg_id=%d)", topic, message, msg_id);
            s_publish_fail_count = 0;                   // ← reset on success
            ok = true;
        }
    }
    xSemaphoreGiveRecursive(get_mqtt_mutex());
    return ok;
}

void MqttClient::forceReconnect() {
    ESP_LOGW(TAG, "forceReconnect() called – tearing down and rebuilding client");
    // Take mutex to safely access teardown_task_handle and scheduleReconnect
    xSemaphoreTakeRecursive(get_mqtt_mutex(), portMAX_DELAY);
    if (teardown_task_handle) {
        xTaskNotify(teardown_task_handle, 0, eNoAction);
    }
    // Schedule reconnect after 1 second (same logic as prolonged disconnect)
    if (mqtt_reconnect_timer == nullptr) {
        mqtt_reconnect_timer = xTimerCreate("mqtt_reconnect", pdMS_TO_TICKS(1000), pdFALSE,
                                            nullptr, mqtt_reconnect_timer_cb);
    } else {
        xTimerStop(mqtt_reconnect_timer, 0);
        xTimerChangePeriod(mqtt_reconnect_timer, pdMS_TO_TICKS(1000), 0);
    }
    xTimerStart(mqtt_reconnect_timer, 0);
    xSemaphoreGiveRecursive(get_mqtt_mutex());
}

void MqttClient::registerReconnectCallback(ReconnectCallback cb) {
    s_reconnect_callback = cb;
}

// ── SAMPLE derived class
// ──────────────────────────────────────────────────────
esp_err_t SAMPLE_derivedMqttClient::create(esp_mqtt_client_config_t config) {
  if (MqttClient::getInstance() != nullptr) return ESP_OK;
  auto *inst = new SAMPLE_derivedMqttClient();
  esp_err_t err = inst->start(config);
  if (err != ESP_OK) {
    delete inst;
    return err;
  }
  MqttClient::setInstance(inst);
  MqttClient::mqttConfig = config;
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

void SAMPLE_derivedMqttClient::send_ping_message(const char *message) {
  publish("IOT_DB/DIAG", message, 1, false);
}

} // namespace ED_MQTT