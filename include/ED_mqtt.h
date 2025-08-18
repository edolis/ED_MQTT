#pragma once
#include <esp_event_base.h>
#include <mqtt_client.h>

namespace ED_MQTT {

/**
 * @brief note! the mosquitto client should connect encrypting the transmitted
credentials using TLS
 * which requires a certificate is loaded on the client side.
 * the certificate ca.crt should be saved in a certs subfolder of the project
and loaded in the firmware by adding to the platformio.ini
 board_build.embed_txtfiles = certs/ca.crt
 *

 NOTE ON ENVIRONMENT SETUP.
 To include a ca.crt
 a) you need to add to the platformio environment configuration
board_build.embed_txtfiles = src/certs/ca.crt
b) you need to modify the cMakeLists.txt int he folder [where main.cpp is
located] adding EMBED_TXTFILES certs/ca.crt  , like in

idf_component_register(SRCS ${app_sources}
                   INCLUDE_DIRS "."
                   EMBED_TXTFILES certs/ca.crt  )
c)  in the source code, the name will be without the indication of the relative
path, that is extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
even if the relative path is certs/ca.crt



 */

class MQTTcomm {
private:
  static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data);
  static inline esp_mqtt_client_handle_t client = nullptr;

public:
  MQTTcomm(esp_mqtt_client_config_t config);
  static void send_ping_message();
  static esp_err_t start();
};

} // namespace ED_MQTT