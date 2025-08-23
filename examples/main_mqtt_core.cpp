/**
 * @file maIN_mqtt_core.cpp
 * @author Emanuele Dolis (emanuele.dolis@gmail.com)
 * @brief CORE MOSQUITTO ED_ configuration
 *
 * Implements a basic connection to a mosquitto server using TLS
 * and the credentials stored in the centralized storage system.
 * Please observe the way cMakeFiles.txt is modified and platformio.ini setup
 * This is a working configuration.
 * @version 0.1
 * @date 2025-08-23
 *
 * @copyright Copyright (c) 2025
 *
 */

#include "ED_esp_err.h"
#include "ED_mqtt.h"
#include "ED_sysInfo.h"
#include "ED_wifi.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "secrets.h"
#include <stdio.h>

using namespace ED_SYSINFO;
using namespace ED_wifi;
using namespace ED_MQTT;

static const char *TAG = "MAIN";

// Embedded CA certificate
extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_crt_end[] asm("_binary_ca_crt_end");

// Global MQTT client handle
// static esp_mqtt_client_handle_t client = nullptr;

extern "C" void app_main(void) {

  dumpSysInfo();
  // dump_ca_cert(ca_crt_start, ca_crt_end);

  // Launch WiFi
  WiFiService::launch(); // Credentials from secrets.h
  // MQTT Configuration

  // static std::string cert(reinterpret_cast<const char *>(ca_crt_start),
  //                         ca_crt_end - ca_crt_start);
  // cert.push_back('\0');

  esp_mqtt_client_config_t mqtt_cfg = {
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
                      .certificate =
                          reinterpret_cast<const char *>(ca_crt_start),
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
              .authentication =
                  {
                      .password = ED_MQTT_PASSWORD,
                  },
          },
  };
  // Initialize MQTT client
  std::unique_ptr<MyMqttClient> MQTTclient;

  // Subscribe to IP ready event
  WiFiService::subscribeToIPReady([&mqtt_cfg, &MQTTclient]() {
    ESP_LOGI(TAG, "***IP is ready! Checking DNS...");
    // print_dns_info();
    // DNSlookup("raspi00");
    // ESP_LOGI(TAG, "CERTPOINTER next");
    // ESP_LOGI(TAG, "CERTPOINTER r: %p, len: %d",
    //          mqtt_cfg.broker.verification.certificate,
    //          mqtt_cfg.broker.verification.certificate_len);
    MQTTclient = MyMqttClient::create(mqtt_cfg);
  });

  while (true) {

    if (MQTTclient) {
      MQTTclient->send_ping_message();
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
