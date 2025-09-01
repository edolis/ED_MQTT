#include "ED_mqtt.h"
#include "ED_sysstd.h"
#include "esp_event_base.h"
#include "secrets.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <iostream>

namespace ED_MQTT {

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

void MqttClient::setDefaultConfig() {
  /**
   * @brief note! required as defining it const creates crashes as there is no
   * guarantee the ED_sysstd are initialized before at boot this will guarantee
   * initialization takes place after app start
   *
   */
  static std::string topicStr =
      "devices/connection/" + std::string(ED_sysstd::ESP_std::mqttName());
  static std::string msgStr = std::string(ED_sysstd::ESP_std::mqttName()) +
                              " disconnected unexpectedly.";

  esp_mqtt_client_config_t cfg = {};
  cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = ED_MQTT_URI,
                  },
              .verification =
                  {
                      .use_global_ca_store = false,
                      .certificate =
                          reinterpret_cast<const char *>(ca_crt_start),
                      .certificate_len = static_cast<size_t>(
                          reinterpret_cast<uintptr_t>(ca_crt_end) -
                          reinterpret_cast<uintptr_t>(ca_crt_start)),
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
                      .use_secure_element=false,
                  },
          },
      .session =
          {
              .last_will =
                  {
                      .topic = topicStr.c_str(),
                      .msg = msgStr.c_str(),
                      .qos = MqttQoS::QOS1,
                      .retain = true,
                  },
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
    client = esp_mqtt_client_init(&config);
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
    ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED — attempting reconnect...");
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT_EVENT_ERROR — transport error");
    break;
  case MQTT_EVENT_DATA:
#ifdef DEBUG_BUILD

    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
#endif
    static std::string partialPayload;
    // Append chunk to buffer
    partialPayload.append(event->data, event->data_len);

    if (event->total_data_len == event->data_len + event->current_data_offset) {
      // Last chunk received
      ESP_LOGI(TAG, "Full message received:");
      // for (size_t i = 0; i < data.length(); ++i) {
      //   printf("[%02X] ", static_cast<unsigned char>(data[i]));
      // }
      // printf("\n");
      ESP_LOGI(TAG, "Raw payload: [%.*s]", partialPayload.length(), partialPayload.c_str());
      ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
      ESP_LOGI(TAG, "DATA=%s", partialPayload.c_str());

      // Process full message
      // processMessage(event->topic, partialPayload);

      for (const auto &callback : data_callbacks) {
   callback(event->client, std::string(event->topic, event->topic_len), partialPayload);

      }

      // Clear buffer
      partialPayload.clear();
    }
  default:
    ESP_LOGI(TAG, "MQTT event id:%s", mqtt_event_names[event->event_id]);
    break;
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
