#include "ED_mqtt.h"
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

// MqttClient.cpp

std::unique_ptr<MqttClient>
MqttClient::create(esp_mqtt_client_config_t config) {
  auto instance = std::make_unique<MqttClient>();
  if (instance->start(config) != ESP_OK) {
    return nullptr;
  }
  return instance;
}

void MqttClient::registerHandler(esp_mqtt_client_handle_t clientHandle) {
  client = clientHandle;
  esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, handleEventCallback,
                                 this);
}

void MqttClient::handleEventCallback(void *handler_args, esp_event_base_t base,
                                     int32_t event_id, void *event_data) {
  // Cast the handler_args back to an instance of the class and call the virtual
  // method.
  MqttClient *instance = static_cast<MqttClient *>(handler_args);
  if (instance) {
    instance->handleEvent(base, event_id, event_data);
  }
}

void MqttClient::handleEvent(esp_event_base_t base, int32_t event_id,
                             void *event_data) {
  auto *event = static_cast<esp_mqtt_event_t *>(event_data);
  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
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
  case MQTT_EVENT_DATA: // data received from the broker on subscribed topics
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\n", event->topic_len, event->topic);
    printf("DATA=%.*s\n", event->data_len, event->data);
    break;
  default:
    ESP_LOGI(TAG, "MQTT event id:%s", mqtt_event_names[event->event_id]);
    break;
  }
}

esp_err_t MqttClient::start(esp_mqtt_client_config_t config) {
  // ... setup and configuration code for the MQTT client ...

  // Get the client handle and register this instance's handler
  // esp_mqtt_client_handle_t client = esp_mqtt_client_get_handle(); // Assuming
  // a way to get the handle
  client = esp_mqtt_client_init(&config);
  // ESP_LOGI(TAG,"registering handler");
  registerHandler(client);
  return esp_mqtt_client_start(client);
}

std::unique_ptr<MqttClient> MyMqttClient::makeInstance() {
  return std::make_unique<MyMqttClient>();
}

std::unique_ptr<MyMqttClient>
MyMqttClient::create(const esp_mqtt_client_config_t &config) {
  // ESP_LOGI(TAG,"in create");
  auto instance = std::make_unique<MyMqttClient>();
  if (instance->start(config) != ESP_OK) {
    return nullptr;
  }
  return instance;
}

void MyMqttClient::handleEvent(esp_event_base_t base, int32_t event_id,
                               void *event_data) {
  // Your specific MQTT event handling logic for this class.
  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "Connected to MQTT broker!");
    break;
  case MQTT_EVENT_DATA:
    messageCounter++;
    ESP_LOGI(TAG, "Received data. Total messages: %d", messageCounter);
    break;
  default:
    MqttClient::handleEvent(base, event_id, event_data);
    break;
  }
};

void MyMqttClient::send_ping_message() {
  int64_t uptime_us = esp_timer_get_time(); // microseconds since boot
  int64_t uptime_ms = uptime_us / 1000;

  char message[64];
  snprintf(message, sizeof(message), "%s:%lld", TAG, uptime_ms);

  esp_mqtt_client_publish(client, "pingESPdevice", message, 0, 1, 0);
  ESP_LOGI(TAG, "Ping sent: %s", message);
}

/*

esp_err_t MQTTcomm::start(esp_mqtt_client_config_t config) {
  client = esp_mqtt_client_init(&config);
  esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, client);
  return esp_mqtt_client_start(client);
}

void MQTTcomm::send_ping_message() {
  int64_t uptime_us = esp_timer_get_time(); // microseconds since boot
  int64_t uptime_ms = uptime_us / 1000;

  char message[64];
  snprintf(message, sizeof(message), "%s:%lld", TAG, uptime_ms);

  esp_mqtt_client_publish(client, "pingESPdevice", message, 0, 1, 0);
  ESP_LOGI(TAG, "Ping sent: %s", message);
}




void MQTTcomm::mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                  int32_t event_id, void *event_data) {
  esp_mqtt_client_handle_t client = (esp_mqtt_client_handle_t)handler_args;
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  switch (event->event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    esp_mqtt_client_subscribe(client, "/test/topic", 0);
    esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32", 0, 1,
                            0);
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED — attempting reconnect...");
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT_EVENT_ERROR — transport error");
    break;
  case MQTT_EVENT_DATA: // data received from the broker on subscribed topics
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\n", event->topic_len, event->topic);
    printf("DATA=%.*s\n", event->data_len, event->data);
    break;
  default:
    ESP_LOGI(TAG, "Other MQTT event id:%d", event->event_id);
    break;
  }
}
*/

} // namespace ED_MQTT
