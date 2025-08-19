#include "ED_mqtt.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <iostream>

namespace ED_MQTT {

static const char *TAG = "ED_MQTT";

ESP_EVENT_DEFINE_BASE(ED_MQTT_SENSOR_EVENTS);

// MqttClient.cpp

  std::unique_ptr<MqttClient> MqttClient::create(esp_mqtt_client_config_t config) {
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
    ESP_LOGI(TAG, "Other MQTT event id:%d", event->event_id);
    break;
  }
}

esp_err_t MqttClient::start(esp_mqtt_client_config_t config) {
  // ... setup and configuration code for the MQTT client ...

  // Get the client handle and register this instance's handler
  // esp_mqtt_client_handle_t client = esp_mqtt_client_get_handle(); // Assuming
  // a way to get the handle
  client = esp_mqtt_client_init(&config);
  registerHandler(client);
  return esp_mqtt_client_start(client);
}

   std::unique_ptr<MqttClient> MyMqttClient::makeInstance() {
    return std::make_unique<MyMqttClient>();
  }


  std::unique_ptr<MyMqttClient> MyMqttClient::create(const esp_mqtt_client_config_t& config) {
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
    std::cout << "Connected to MQTT broker!" << std::endl;
    break;
  case MQTT_EVENT_DATA:
    messageCounter++;
    std::cout << "Received data. Total messages: " << messageCounter
              << std::endl;
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
