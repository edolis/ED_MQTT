#include "ED_mqtt_CRTP.h"
// #include "esp_event_base.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <iostream>

namespace ED_MQTT {

static const char *TAG = "ED_MQTT";

/* already defined in ED_MQTT.h

const char *mqtt_event_names[] = {
    ...
};

*/
// ESP_EVENT_DEFINE_BASE(ED_MQTT_SENSOR_EVENTS);

// MqttClient_CRTP.cpp
/**
 * note! create initializes the only instance of the class and saves in the
 * static _instance Must be redeclared in derived class to use the static
 * _instance variable of the derived class
 */
// Derived* MqttClient_CRTP::create(esp_mqtt_client_config_t config)

// static trampoline callback method to redirect to the instance override-able
// handler
// void MqttClient_CRTP::mqtt_event_trampoline(void *handler_args,
//                                        esp_event_base_t base, int32_t event_id,
//                                        void *event_data)


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


void SAMPLE_derivedMqttClient_CRTP::send_ping_message() {
  int64_t uptime_us = esp_timer_get_time(); // microseconds since boot
  int64_t uptime_ms = uptime_us / 1000;

  char message[64];
  snprintf(message, sizeof(message), "%s:%lld", TAG, uptime_ms);

  esp_mqtt_client_publish(client, "pingESPdevice", message, 0, 1, 0);
  ESP_LOGI(TAG, "Ping sent: %s", message);
}

} // namespace ED_MQTT
