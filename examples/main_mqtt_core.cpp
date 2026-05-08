/**
 * @file main_mqtt_core.cpp
 * @brief MQTT over TLS client for ESP32 using custom ED_* libraries.
 *        Connects to a Mosquitto broker with credentials from secrets.h.
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version GIT_VERSION: v0.0.1-1-g5ab3ca2-dirty
 * @date 2026-05-08
 * @submodules-start
  *   ED_MQTT   : v1.1.0-4-gbc7c1c0-dirty
  *   ED_S_JSON : v1.1.0-0-g62ddf73
  *   ED_WIFI   : v1.0.0-0-g2f08383
 * @submodules-end
*/


#include <cstdio>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


// Custom application libraries
#include "ED_S_JSON.h" // Static JSON builder
#include "ED_esp_err.h"
#include "ED_heap_audit.h" // Heap snapshot utilities
#include "ED_mqtt.h"
#include "ED_sys.h"
#include "ED_wifi.h"


#include "secrets.h" // WiFi & MQTT credentials
#include "version.h" // Build version information

// -----------------------------------------------------------------------------
//  Build Information (placeholders – replace with your actual versioning)
// -----------------------------------------------------------------------------
namespace ED_SYSINFO {
struct GIT_fwInfo {
  static constexpr const char *GIT_VERSION = "v0.0.1-0-g20d68aa-dirty";
  static constexpr const char *GIT_TAG = "v0.0.1";
  static constexpr const char *GIT_HASH = "g20d68aa";
  static constexpr const char *FULL_HASH =
      "0000000000000000000000000000000000000000";
  static constexpr const char *BUILD_ID = "P20260508-134144-20d68aa";
};
} // namespace ED_SYSINFO

using namespace ED_MQTT;
// -----------------------------------------------------------------------------
//  Constants
// -----------------------------------------------------------------------------
static const char *TAG = "ESP_main_loop";

/** JSON buffer size for the ping payload (must be large enough for all fields)
 */
#ifndef JSON_BUFFER_SIZE
#define JSON_BUFFER_SIZE 1024
#endif

/** Ping interval in milliseconds */
static constexpr int PING_INTERVAL_MS = 10000;

// -----------------------------------------------------------------------------
//  Utility Functions
// -----------------------------------------------------------------------------

/**
 * Convert seconds to a compact human‑readable string: "xxd00h00m"
 * @param seconds  total uptime in seconds
 * @return pointer to a static buffer (reused on each call)
 */
static const char *formatUptime(int64_t seconds) {
  static char buf[16]; // enough for "999d23h59m\0"
  const uint16_t days = static_cast<uint16_t>(seconds / 86400);
  const uint8_t hours = static_cast<uint8_t>((seconds % 86400) / 3600);
  const uint8_t minutes = static_cast<uint8_t>((seconds % 3600) / 60);
  snprintf(buf, sizeof(buf), "%dd%02dh%02dm", days, hours, minutes);
  return buf;
}
/**
 * Build a JSON ping payload containing:
 * - Root diagnostic (DTF): uptime, WiFi info (always saved)
 * - Additional diagnostic block (DTM): heap metrics (saved only if dS = "S")
 * Uses a static internal buffer – no dynamic allocation.
 * @return pointer to a null‑terminated JSON string (empty string on error)
 */
static const char *buildPingPayload() {
  const int64_t uptime_sec = esp_timer_get_time() / 1000000LL;
  const auto wifiInfo = ED_wifi::WiFiService::getCurrentAPInfo();

  // Take a heap snapshot (non‑allocating)
  heap_audit_snapshot_t heap_snap;
  heap_audit_take_snapshot(&heap_snap);

  // Static JSON builder – reuses the same internal buffer
  static ED_S_JSON::StaticJson json;
  json.beginObject();

  // ----- Root diagnostic block (DTF) – always saved -----
  json.addInt("d_UPS", static_cast<uint64_t>(uptime_sec));
  json.addString("d_UPT", formatUptime(uptime_sec));
  json.addString("dNM", ED_SYS::ESP_std::Device::mqttName());
  json.addString("dDGT", "DTF");          // root diagnostic type

  if (wifiInfo.has_value()) {
    json.addInt("d_rssi_dbm", static_cast<int>(wifiInfo->rssi));
    json.addString("d_rssi_ID", wifiInfo->ssid);
  }

  // ----- Additional diagnostics array (child blocks) -----
  json.beginArray("diagnostics");
  {
    // Heap diagnostic block (DTM) – will be saved when dS = "S"
    json.beginObject();                   // start of entry
    json.addString("dDGT", "DTM");        // child diagnostic type
    json.addString("dS", "S");            // save flag – always save this block
    // Heap metrics with d_ prefix (will become columns in dia_DTF_DTM)
    json.addInt("d_hfree", heap_snap.total_free_bytes);
    json.addInt("d_hlarge", heap_snap.largest_free_block);
    json.addInt("d_hall", heap_snap.total_allocated_bytes);
    json.addInt("d_hblks", heap_snap.allocated_blocks);
    json.addInt("d_hfblk", heap_snap.free_blocks);
    json.addInt("d_hfrag", static_cast<int>(heap_snap.fragmentation_percent));
    json.addInt("d_hmin8", heap_snap.min_free_8bit);
    json.addInt("d_hmin32", heap_snap.min_free_32bit);
    json.addBool("d_hintg", heap_snap.heap_integrity_ok);
    json.endObject();
  }
  json.endArray();                        // end of diagnostics

  json.endObject();
  return json.toString();
}

// -----------------------------------------------------------------------------
//  Main Application Entry Point
// -----------------------------------------------------------------------------
extern "C" void app_main(void) {
  // Start WiFi (credentials are taken from secrets.h)
  ED_wifi::WiFiService::launch();

  // MQTT client configuration (TLS, credentials, will message)
  esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = "mqtts://raspi00:8883",
                  },
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
                      .topic = "test",
                      .msg = "last will message",
                      .qos = 1,
                      .retain = true,
                  },
              .protocol_ver = MQTT_PROTOCOL_V_5,
          },
  };

  // When WiFi obtains an IP, create the MQTT client
  ED_wifi::WiFiService::subscribeToIPReady([&mqtt_cfg]() {
    ESP_LOGI(TAG, "IP ready – creating MQTT client");
    const esp_err_t err = SAMPLE_derivedMqttClient::create(mqtt_cfg);
    ESP_LOGI(TAG, "MQTT create returned: %s", esp_err_to_name(err));
  });

  // Main loop: send a ping every PING_INTERVAL_MS milliseconds
  while (true) {
    auto *mqtt = SAMPLE_derivedMqttClient::getInstance();
    if (mqtt && mqtt->getHandle() != nullptr) {
      const char *payload = buildPingPayload();
      if (payload && payload[0] != '\0') {
        mqtt->send_ping_message(payload);
      } else {
        ESP_LOGW(TAG, "Empty payload, skipping ping");
      }
    } else {
      ESP_LOGI(TAG, "MQTT client not ready, waiting...");
    }
    vTaskDelay(pdMS_TO_TICKS(PING_INTERVAL_MS));
  }
}