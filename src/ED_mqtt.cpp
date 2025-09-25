#include "ED_mqtt.h"
#include "ED_heap_tracer.h"
#include "ED_sys.h"
#include "esp_crt_bundle.h"
#include "esp_event_base.h"
#include "mqtt5_client.h"
#include "secrets.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>
#include <iostream>


namespace ED_MQTT {

static TimerHandle_t mqtt_reconnect_timer = nullptr;
// ED_mqtt.cpp
TaskHandle_t ED_MQTT::MqttClient::teardown_task_handle = nullptr;

void MqttClient::mqtt_reconnect_timer_cb(TimerHandle_t xTimer) {
  ESP_LOGI("MQTT", "Reconnect timer fired — creating new client");
  // Call your existing init/start logic here
  getInstance()->start(mqttConfig);
}

static const char *TAG = "ED_MQTT";

const char *mqtt_event_names[] = {
    /*MQTT_EVENT_ERROR*/ "MQTT_EVENT_ERROR", //: on error event, additional
                                             //: context: connection return
                                             //: code, error handle from esp_tls
                                             //: (if supported)
    /*MQTT_EVENT_CONNECTED*/ "MQTT_EVENT_CONNECTED", //: connected event,
                                                     //: additional context:
                                                     //: session_present flag
    /*MQTT_EVENT_DISCONNECTED*/ "MQTT_EVENT_DISCONNECTED", //: disconnected
                                                           //: event
    /*MQTT_EVENT_SUBSCRIBED*/
    "MQTT_EVENT_SUBSCRIBED", //: subscribed event, additional context:
                             //  - msg_id               message id
                             //  - error_handle         `error_type` in case
                             //  subscribing failed
                             //  - data                 pointer to broker
                             //  response, check for errors.
                             //  - data_len             length of the data for
                             //  this event
    /*MQTT_EVENT_UNSUBSCRIBED*/ "MQTT_EVENT_UNSUBSCRIBED", //: unsubscribed
                                                           //: event, additional
                                                           //: context:  msg_id
    /*MQTT_EVENT_PUBLISHED*/ "MQTT_EVENT_PUBLISHED",       //: published event,
                                                     //: additional context:
                                                     //: msg_id
    /*MQTT_EVENT_DATA*/
    "MQTT_EVENT_DATA", //: data event, additional context:
                       //  - msg_id               message id
                       //  - topic                pointer to the received topic
                       //  - topic_len            length of the topic
                       //  - data                 pointer to the received data
                       //  - data_len             length of the data for this
                       //  event
                       //  - current_data_offset  offset of the current data for
                       //  this event
                       //  - total_data_len       total length of the data
                       //  received
                       //  - retain               retain flag of the message
                       //  - qos                  QoS level of the message
                       //  - dup                  dup flag of the message
                       //  Note: Multiple MQTT_EVENT_DATA could be fired for one
                       //  message, if it is longer than internal buffer. In
                       //  that case only first event contains topic pointer and
                       //  length, other contain data only with current data
                       //  length and current data offset updating.
    /*MQTT_EVENT_BEFORE_CONNECT*/ "MQTT_EVENT_BEFORE_CONNECT", //: The event
                                                               //: occurs before
                                                               //: connecting
    /*MQTT_EVENT_DELETED*/
    "MQTT_EVENT_DELETED", //: Notification on delete of one message from the
                          //: internal outbox, if the message couldn't have been
                          //: sent and acknowledged before expiring defined in
                          //: OUTBOX_EXPIRED_TIMEOUT_MS. (events are not posted
                          //: upon deletion of successfully acknowledged
                          //: messages)
                          //  - This event id is posted only if
                          //  MQTT_REPORT_DELETED_MESSAGES==1
                          //  - Additional context: msg_id (id of the deleted
                          //  message).
    /*MQTT_USER_EVENT*/ "MQTT_USER_EVENT", //: Custom event used to queue tasks
                                           //: into mqtt event handler
                                           //  All fields from the
                                           //  esp_mqtt_event_t type could be
                                           //  used to pass an additional
                                           //  context data to the handler.

};

ESP_EVENT_DEFINE_BASE(ED_MQTT_SENSOR_EVENTS);
/*
const esp_mqtt_client_config_t MqttClient::mqtt_default_cfg = {

    .broker =
        {
            .address =
                {
                    .uri = ED_MQTT_URI,

                    //.hostname = "raspi00",
                },
            .verification =
                {
                    .use_global_ca_store = false,
                    // .certificate = cert.c_str(),
                    // .certificate_len = cert.length(),
                    .certificate = reinterpret_cast<const char *>(ca_crt_start),
                    .certificate_len =
                        // static_cast<size_t>(ca_crt_end - ca_crt_start),
                    static_cast<size_t>(
                        reinterpret_cast<uintptr_t>(ca_crt_end) -
                        reinterpret_cast<uintptr_t>(ca_crt_start)), // to
                    //  avoid warning in code analysis, conversion to ram
                    //  memory pointers.

                    .skip_cert_common_name_check = false,
                },
        },
    .credentials =
        {
            .username = ED_MQTT_USERNAME,
            .client_id = ED_sysstd::ESP_std::mqttName(),
            .authentication =
                {
                    .password = ED_MQTT_PASSWORD,
                },
        },
    .session = {.last_will{
        .topic = ("devices/connection/" +
                  std::string(ED_sysstd::ESP_std::mqttName()))
                     .c_str(),
        .msg = (std::string(ED_sysstd::ESP_std::mqttName()) +
                " disconnected unexpectedlty.")
                   .c_str(),
        .qos = MqttQoS::QOS1,
        .retain = true,
    }},
};
*/

int64_t MqttClient::mqtt5_get_epoch_property(const esp_mqtt_event_t *event) {
#ifdef CONFIG_MQTT_PROTOCOL_5
  // Early validation
  if (!event || !event->property || !event->property->user_property) {
    // if(!event)  ESP_LOGI(TAG,"Step_event null");
    //  else if (!event->property) ESP_LOGI(TAG,"Step_event->property null");
    //  else if (!event->property->user_property)
    //  ESP_LOGI(TAG,"Step_event->property->user_property  null" );
    return -1;
  }

  mqtt5_user_property_handle_t handle = event->property->user_property;
  uint8_t count = esp_mqtt5_client_get_user_property_count(handle);
  // ESP_LOGI(TAG,"Step_count is %d",count);
  if (count == 0) {
    return -1;
  }

  // Use stack allocation for small property counts to avoid malloc/free
  constexpr uint8_t STACK_THRESHOLD = 4;

  if (count <= STACK_THRESHOLD) {
    // Stack allocation - much faster for typical cases
    esp_mqtt5_user_property_item_t items[STACK_THRESHOLD];
    if (esp_mqtt5_client_get_user_property(handle, items, &count) == ESP_OK) {
      for (uint8_t i = 0; i < count; i++) {
        //  ESP_LOGI(TAG, "***Key: %s, Value: %s", items[i].key,
        //  items[i].value);
        if (items[i].key && strcmp(items[i].key, "epoch") == 0) {
          return atoll(items[i].value);
        }
      }
    }
  } else {
    // Heap allocation only for larger arrays
    esp_mqtt5_user_property_item_t *items =
        (esp_mqtt5_user_property_item_t *)malloc(
            sizeof(esp_mqtt5_user_property_item_t) * count);
    if (!items) {
      return -1;
    }

    if (esp_mqtt5_client_get_user_property(handle, items, &count) == ESP_OK) {
      for (uint8_t i = 0; i < count; i++) {
        if (items[i].key && strcmp(items[i].key, "epoch") == 0) {
          int64_t result = atoll(items[i].value);
          free(items);
          return result;
        }
      }
    }
    free(items);
  }

  return -1;
#else
  return -1;
#endif
}

void MqttClient::setDefaultConfig() {
  /**
   * @brief note! required as defining it const creates crashes as there is no
   * guarantee the ED_sysstd are initialized before at boot this will guarantee
   * initialization takes place after app start
   *
   */
  static char topicBuf[64]; // adjust size as needed
  static char msgBuf[64];   // adjust size as needed
  snprintf(topicBuf, sizeof(topicBuf), "devices/connection/%s",
           ED_SYS::ESP_std::Device::mqttName());
  snprintf(msgBuf, sizeof(msgBuf), "%s disconnected unexpectedly.",
           ED_SYS::ESP_std::Device::mqttName());

  esp_mqtt_client_config_t cfg = {};
  cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = ED_MQTT_URI,
                  },
              // .verification =
              //     {
              //         .use_global_ca_store = false,
              //         .certificate =
              //             reinterpret_cast<const char *>(ca_crt_start),
              //         .certificate_len = static_cast<size_t>(
              //             reinterpret_cast<uintptr_t>(ca_crt_end) -
              //             reinterpret_cast<uintptr_t>(ca_crt_start)),
              //         .skip_cert_common_name_check = false,
              //     },
              .verification =
                  {
                      .use_global_ca_store = false,
                      .crt_bundle_attach = esp_crt_bundle_attach,
                      .skip_cert_common_name_check = false,
                  },
          },
      .credentials =
          {
              .username = ED_MQTT_USERNAME,
              .client_id = ED_SYS::ESP_std::Device::mqttName(),
              .authentication =
                  {
                      .password = ED_MQTT_PASSWORD,
                      .use_secure_element = false,
                  },
          },
      .session =
          {
              .last_will =
                  {
                      .topic = topicBuf,
                      .msg = msgBuf,
                      .qos = MqttQoS::QOS1,
                      .retain = true,
                  },
              .protocol_ver = esp_mqtt_protocol_ver_t::MQTT_PROTOCOL_V_5,
          },
  };
  mqttConfig = cfg;
}

// MqttClient.cpp
/**
 * note! create initializes the only instance of the class and saves in the
 * static _instance Must be redeclared in derived class to use the static
 * _instance variable of the derived class
 */
MqttClient *MqttClient::create(esp_mqtt_client_config_t *config) {

  xTaskCreate(teardown_task, "mqtt_teardown",
              4096, // stack size
              NULL, // task parameter
              tskIDLE_PRIORITY + 1, &teardown_task_handle);

  if (_instance == nullptr) { // avoid retrying to initialize when already done
    if (config != nullptr) {
      mqttConfig = *config;
    } else
      setDefaultConfig();
    auto instance = std::make_unique<MqttClient>();
    esp_err_t err = instance->start(mqttConfig);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
      return nullptr;
    }
    _instance = move(instance);
  } else {
    // if( configuration equals)
    /*
    bool areConfigsEqual(const esp_mqtt_client_config_t& cfg1, const
esp_mqtt_client_config_t& cfg2) { if (strcmp(cfg1.broker.address.uri,
cfg2.broker.address.uri) != 0) return false; if
(cfg1.broker.verification.use_global_ca_store !=
cfg2.broker.verification.use_global_ca_store) return false; if
(cfg1.broker.verification.certificate_len !=
cfg2.broker.verification.certificate_len) return false; if
(memcmp(cfg1.broker.verification.certificate,
cfg2.broker.verification.certificate, cfg1.broker.verification.certificate_len)
!= 0) return false; if (strcmp(cfg1.credentials.username,
cfg2.credentials.username) != 0) return false; if
(strcmp(cfg1.credentials.client_id, cfg2.credentials.client_id) != 0) return
false; if (strcmp(cfg1.credentials.authentication.password,
cfg2.credentials.authentication.password) != 0) return false; if
(strcmp(cfg1.session.last_will.topic, cfg2.session.last_will.topic) != 0) return
false; if (strcmp(cfg1.session.last_will.msg, cfg2.session.last_will.msg) != 0)
return false; if (cfg1.session.last_will.qos != cfg2.session.last_will.qos)
return false; if (cfg1.session.last_will.retain !=
cfg2.session.last_will.retain) return false;

    return true;
}

*/
    // TODO implement reconnection with different config
  }
  return _instance.get();
}

esp_mqtt_client_handle_t MqttClient::getHandle() {
  // ESP_LOGI(TAG, "in gethandle, client: %s ",
  //          (client == nullptr) ? "nullptr" : "OK");
  return client;
};
// static trampoline callback method to redirect to the instance override-able
// handler
void MqttClient::mqtt_event_trampoline(void *handler_args,
                                       esp_event_base_t base, int32_t event_id,
                                       void *event_data) {
  auto *self = static_cast<MqttClient *>(handler_args);
  self->handleEvent(base, event_id,
                    event_data); // note! self for polimorphysm will be the
  // picked from the most derived class despite cast to the base class
}

esp_err_t MqttClient::start(esp_mqtt_client_config_t config) {
  // ... setup and configuration code for the MQTT client
  // notice just one shared client will be created in the base class
  // in order to optimize resources.
  // multiple client configuration could also be used , to manage different QoS
  // etc. but that would consume additional resources and increase complexity
  esp_err_t err;
  // TODO need initialize clientID with controlled value
  if (client == nullptr) { //
    // can initialize just once. Since multiple derived classes and/or the base
    // classes share the same mqtt client, they just need to register their
    // event handlers
    ESP_LOGI(TAG, "Free heap: %d, Largest block: %d",
             heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
    ED_heap_tracer::mqtt_heap_trace_start();
    client = esp_mqtt_client_init(&config);
    heap_caps_print_heap_info(MALLOC_CAP_DEFAULT);
    if (client == nullptr)
      return ESP_FAIL;
    err = esp_mqtt_client_start(client);
    if (err != ESP_OK)
      return err;
  }

  // ESP_LOGI(TAG,"registering handler");
  if (!eventsRegistered) {
    err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                         mqtt_event_trampoline, this);
    eventsRegistered = true;
    return err;
  }
  return ESP_OK;
}

void MqttClient::scheduleReconnect(uint32_t delay_ms) {
  if (mqtt_reconnect_timer == nullptr) {
    mqtt_reconnect_timer =
        xTimerCreate("mqtt_reconnect", pdMS_TO_TICKS(delay_ms),
                     pdFALSE, // one-shot
                     nullptr, mqtt_reconnect_timer_cb);
  } else {
    xTimerStop(mqtt_reconnect_timer, 0);
    xTimerChangePeriod(mqtt_reconnect_timer, pdMS_TO_TICKS(delay_ms), 0);
  }
  xTimerStart(mqtt_reconnect_timer, 0);
}

void MqttClient::teardown_task(void *arg) {
  auto *self = static_cast<MqttClient *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (self->client) {
      ESP_LOGW(TAG, "Destroying MQTT client from safe context");
      esp_mqtt_client_stop(self->client);
      esp_mqtt_client_destroy(self->client);
      self->client = nullptr;
    }
  }
}



void MqttClient::handleEvent(esp_event_base_t base, int32_t event_id,
                             void *event_data) {
  auto *event = static_cast<esp_mqtt_event_t *>(event_data);
  // note. base will always be the same value, the MQQT_EVENT or so- not clear
  // about the value, but it's a dead parameter here

  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    // manages must haqve subscriptions. the remaining ones required by external
    // classes can be activated using the connected_callbacks
    esp_mqtt_client_subscribe(client, "devices/connection", 0);
#ifdef DEBUG_BUILD
    esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32", 0,
                            MqttQoS::QOS1, 0);
#endif
    // Iterate and call all registered subscriber functions
    for (const auto &callback : connected_callbacks) {
      callback(event->client);
    }
    break;
  case MQTT_EVENT_DISCONNECTED:
    if (isShortOutage()) {
      // Let built-in reconnect do its thing
      ESP_LOGW(TAG, "Transient disconnect — letting MQTT auto-reconnect");
    } else {
      // Hard failure — free everything
      ESP_LOGE(TAG, "Prolonged/fatal disconnect — tearing down client");
      ED_heap_tracer::mqtt_heap_trace_snapshot_async(); // snapshot here too
      destroyClient();
      scheduleReconnect(3000); // create a fresh client after delay
    }
    ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED — attempting reconnect...");
    break;
  case MQTT_EVENT_ERROR:
    ED_heap_tracer::mqtt_heap_trace_snapshot_async(); // snapshot here too
    xTaskNotify(teardown_task_handle, 0, eNoAction);
    ESP_LOGE(TAG, "MQTT_EVENT_ERROR — transport error");
    break;
  case MQTT_EVENT_DATA: {
    static std::string partialPayload;
    static size_t expected_len = 0;


    // First chunk of a new message
    if (event->current_data_offset == 0) {
      partialPayload.clear();
      expected_len = event->total_data_len;

      // Reserve once to avoid multiple reallocations
      if (expected_len > 0 && expected_len < 64 * 1024) {
        partialPayload.reserve(expected_len);
      }
    }

    // Cap growth to avoid runaway on malformed streams
    constexpr size_t MAX_PAYLOAD = 16 * 1024;
    if (partialPayload.size() + event->data_len <= MAX_PAYLOAD) {
      partialPayload.append(event->data, event->data_len);
    } else {
      ESP_LOGW(TAG, "Payload too large, discarding");
      partialPayload.clear();
      expected_len = 0;
      break;
    }

    // Last chunk?
    if (event->total_data_len == event->current_data_offset + event->data_len) {
      // Avoid constructing a new string if you can pass pointer+len
      std::string topic(event->topic, event->topic_len);

      // Callbacks should take const std::string& or (const char*, size_t)
      for (auto &cb : data_callbacks) {
        cb(event->client, event->topic, event->topic_len, partialPayload,
           mqtt5_get_epoch_property(event));
      }

      partialPayload.clear();
      expected_len = 0;
    };
    break; ///
  }

default: {
  int id = event->event_id;
  const char *name = nullptr;
  if (id >= 0 &&
      id < (int)(sizeof(mqtt_event_names) / sizeof(mqtt_event_names[0]))) {
    name = mqtt_event_names[id];
  } else {
    name = "MQTT_EVENT_UNKNOWN";
  }
  ESP_LOGI(TAG, "MQTT event id:%d (%s)", id, name);
  break;
}
}
}

MqttClient::~MqttClient() {

  // Unregister event handlers
  if (eventsRegistered && client != nullptr) {
    esp_mqtt_client_unregister_event(client, MQTT_EVENT_ANY,
                                     mqtt_event_trampoline);
    eventsRegistered = false;
  }

  // Destroy MQTT client
  if (client != nullptr) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
}
void MqttClient::destroyClient() {
  if (client) {
    ESP_LOGW(TAG, "Destroying MQTT client and freeing resources");
    esp_mqtt_client_stop(client);    // stop task & close transport
    esp_mqtt_client_destroy(client); // free all buffers & handle
    client = nullptr;
  }
}

bool MqttClient::isShortOutage() {
  int64_t now = esp_timer_get_time() / 1000000; // seconds
  int64_t since_last = now - last_disconnect_time;

  // Update tracking
  last_disconnect_time = now;
  disconnect_count++;

  // Define your thresholds
  const int MAX_DISCONNECTS = 3;   // tolerate up to 3 quick drops
  const int SHORT_WINDOW_SEC = 60; // within 60 seconds

  if (since_last > SHORT_WINDOW_SEC) {
    // Reset counter if last drop was long ago
    disconnect_count = 1;
    return true; // treat as short outage
  }

  if (disconnect_count <= MAX_DISCONNECTS) {
    return true; // still within tolerance
  }

  // Too many quick failures → treat as hard outage
  return false;
}

/**
 * periodic state transmission
 * to be transmitted at connection and periodically
 *
 * a) device hostname
 * b) device MAC (used as key in database ops)
 * c) Wifi conn data (current + available APs, signal strength)
 * d) firmware version
 *
 */

/**
 * note! create initializes the only instance of the class and saves in the
 * static _instance Must be redeclared in derived class to use the static
 * _instance variable of the derived class
 */
esp_err_t SAMPLE_derivedMqttClient::create(esp_mqtt_client_config_t config) {
  // TODO set clientID, improve handling of config

  auto instance = std::make_unique<SAMPLE_derivedMqttClient>();
  esp_err_t err = instance->start(config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    return err;
  }
  _instance = move(instance);
  return ESP_OK;
}

std::unique_ptr<MqttClient> SAMPLE_derivedMqttClient::makeInstance() {
  return std::make_unique<SAMPLE_derivedMqttClient>();
}

void SAMPLE_derivedMqttClient::handleEvent(esp_event_base_t base,
                                           int32_t event_id, void *event_data) {

  // these override the default event handler of the base class
  auto *event = static_cast<esp_mqtt_event_t *>(event_data);
  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    esp_mqtt_client_subscribe(client, "/test/topic", 0);
    esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32", 0,
                            MqttQoS::QOS1, 0);
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED — attempting reconnect...");
    break;
  case MQTT_EVENT_ERROR:

    ESP_LOGE(TAG, "MQTT_EVENT_ERROR — transport error");
    break;
  case MQTT_USER_EVENT:
    ESP_LOGE(TAG,
             "MQTT_USER EVENT"); // this is the place where discriminate payload
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    static std::string partialPayload;
    // Append chunk to buffer
    partialPayload.append(event->data, event->data_len);

    if (event->total_data_len == event->data_len + event->current_data_offset) {
      // Last chunk received
      ESP_LOGI(TAG, "Full message received:");
      ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
      ESP_LOGI(TAG, "DATA=%s", partialPayload.c_str());

      // Process full message
      // processMessage(event->topic, partialPayload);
      ESP_LOGI(TAG, "TOPIC=%s  DATA=%s\n", event->topic,
               partialPayload.c_str());
      // Clear buffer
      partialPayload.clear();
    }
    break;

  default:
    MqttClient::handleEvent(base, event_id, event_data);
    break;
  };
};

void SAMPLE_derivedMqttClient::send_ping_message() {
  int64_t uptime_us = esp_timer_get_time(); // microseconds since boot
  int64_t uptime_ms = uptime_us / 1000;

  char message[64];
  snprintf(message, sizeof(message), "%s:%lld", TAG, uptime_ms);

  esp_mqtt_client_publish(client, "pingESPdevice", message, 0, 1, 0);
  ESP_LOGI(TAG, "Ping sent: %s", message);
}

} // namespace ED_MQTT
