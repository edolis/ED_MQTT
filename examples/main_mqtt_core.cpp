// #region StdManifest
/**
 * @file main_mqtt_core.cpp
 * @brief
 * * Implements a basic connection to a mosquitto server using TLS
 * and the credentials stored in the centralized storage system.
 * Please observe the way cMakeFiles.txt is modified and platformio.ini setup
 * This is a working configuration.
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version GIT_VERSION: v1.0.0.0-0-dirty
 * @tagged as: SNTP-core
 * @commit hash: g5d100c9 [5d100c9e7fbf8030cd9e50ec7db3b7b6333dbee1]
 * @build ID: P20250829-105812-5d100c9
 *  @compiledSizeInfo begin

    .iram0.text      85 214    .dram0.data  11 780
    .flash.text     737 184    .dram0.bss   20 320
    .flash.appdesc      256    ―――――――――――――――――――
    .flash.rodata   128 484    total        32 100
    ―――――――――――――――――――――――                       
    subtotal        951 138                       

    @compiledSizeInfo end
 * @date 2025-08-29
 */

static const char *TAG = "ESP_main_loop";

// #region BuildInfo
namespace ED_SYSINFO {
// compile time GIT status
struct GIT_fwInfo {
    static constexpr const char* GIT_VERSION = "v1.0.0.0-0-dirty";
    static constexpr const char* GIT_TAG     = "SNTP-core";
    static constexpr const char* GIT_HASH    = "g5d100c9";
    static constexpr const char* FULL_HASH   = "5d100c9e7fbf8030cd9e50ec7db3b7b6333dbee1";
    static constexpr const char* BUILD_ID    = "P20250829-105812-5d100c9";
};
} // namespace ED_SYSINFO
// #endregion
// #endregion


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
              .client_id = "ESP32_XX",
              .authentication =
                  {
                      .password = ED_MQTT_PASSWORD,
                  },
          },
  };
  // Initialize MQTT client
  SAMPLE_derivedMqttClient* MQTTclient;

  // Subscribe to IP ready event
  WiFiService::subscribeToIPReady([&mqtt_cfg, &MQTTclient]() {
    ESP_LOGI(TAG, "***IP is ready! Checking DNS...");
    // print_dns_info();
    // DNSlookup("raspi00");
    // ESP_LOGI(TAG, "CERTPOINTER next");
    // ESP_LOGI(TAG, "CERTPOINTER r: %p, len: %d",
    //          mqtt_cfg.broker.verification.certificate,
    //          mqtt_cfg.broker.verification.certificate_len);
    SAMPLE_derivedMqttClient::create(mqtt_cfg);
     MQTTclient = SAMPLE_derivedMqttClient::getInstance();
  });

  while (true) {

    if (MQTTclient) {
      MQTTclient->send_ping_message();
    }
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}
