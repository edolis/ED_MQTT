#include "ED_mqtt.h"
#include <esp_log.h>
#include <esp_timer.h>

namespace ED_MQTT {

static const char *TAG = "ED_MQTT";

MQTTcomm::MQTTcomm(esp_mqtt_client_config_t config) {
  client = esp_mqtt_client_init(&config);
  esp_mqtt_client_register_event(client, (esp_mqtt_event_id_t)ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, client);
}
esp_err_t MQTTcomm::start() {
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
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\n", event->topic_len, event->topic);
    printf("DATA=%.*s\n", event->data_len, event->data);
    break;
  default:
    ESP_LOGI(TAG, "Other MQTT event id:%d", event->event_id);
    break;
  }
}

} // namespace ED_MQTT
