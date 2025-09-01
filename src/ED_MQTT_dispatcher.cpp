#include "ED_MQTT_dispatcher.h"
#include "ED_json.h"
#include <esp_log.h>

namespace ED_MQTT_dispatcher {

static const char *TAG = "ED_MQTT_dispatcher";
TaskHandle_t MQTTdispatcher::infoPublisherTaskHandle = nullptr;
std::vector<iCommand *> MQTTdispatcher::cmdSubscribers = {};
// #region MQTTdispatcher

MqttConnectedCallback MQTTdispatcher::onMqttConnected =
    [](esp_mqtt_client_handle_t client) {
      // sends the default connection message on the connection topic with
      // retain message on
      esp_mqtt_client_publish(
          client, ("devices/connection/" + std::string(myMqttID)).c_str(),
          (std::string(myMqttID) + " connects.").c_str(), 0,
          MqttClient::MqttQoS::QOS1, true);
      // subscribes to the command channel
      esp_mqtt_client_subscribe(client, "cmd", 0);

      // sends the standard device info in the info channel
      esp_mqtt_client_publish(
          client, ("info/connection/" + std::string(myMqttID)).c_str(),
          stdPingJson().c_str(), 0, MqttClient::MqttQoS::QOS1, true);
    };

MqttDataCallback MQTTdispatcher::onMqttData =
    [](esp_mqtt_client_handle_t client, std::string topic, std::string data) {
      ESP_LOGI(TAG, "DISPATCHER Received topic [%s] data [%s] datalength %d",
               topic.c_str(), data.c_str(), data.length());

      // for (size_t i = 0; i < data.length(); ++i) {
      //   printf("[%02X] ", static_cast<unsigned char>(data[i]));
      // }
      // printf("\n");
      // ESP_LOGI(TAG, "Raw payload: [%.*s]", data.length(), data.c_str());

      ED_JSON::JsonEncoder decoder(data);
      if(decoder.isValidJson()) ESP_LOGI(TAG,"decoder creator turns %s into %s",data.c_str(),decoder.getJson().c_str() );
      // unpacks the array in case the client sends json as a json array
      auto unwrapped = decoder.unwrapNestedArray();
      ESP_LOGI(TAG, "unwrapper array?  %d", unwrapped.isArray());
      if (unwrapped.isArray()) {
        int size = unwrapped.getArraySize();
        ESP_LOGI(TAG, "array size %d", size);
        for (int i = 0; i < size; ++i) {
          auto item = unwrapped.getArrayItem(i);
          ESP_LOGI(TAG, "Calling with item %d :  %s", i, item.getCompactJson().c_str());
          handleCommandObject(item);
        }
      } else if (unwrapped.isValidJson()) {
        handleCommandObject(unwrapped);
      } else {
        ESP_LOGW(TAG, "Malformed or unsupported payload: [%s]", data.c_str());
      }
      // publish back as test
      // esp_mqtt_client_publish(
      //     clHandle, ("info/ESPreceived/" + std::string(myMqttID)).c_str(),
      //     ("sending back payload: " + data).c_str(), 0,
      //     MqttClient::MqttQoS::QOS1, false);
    };

void MQTTdispatcher::handleCommandObject(const ED_JSON::JsonEncoder &obj) {
  // ESP_LOGI(TAG,"Step_2 isvalidjson %d",obj.isValidJson());
  if (!obj.isValidJson())
    return;
// ESP_LOGI(TAG,"Step_3");
  auto commandOpt = obj.getString("cmd");
  auto dataOpt = obj.getString("data");

  if (commandOpt && dataOpt) {
    for (auto *sub : cmdSubscribers) {
      // ESP_LOGI(TAG, "calling");
      sub->grabCommand(*commandOpt, *dataOpt);
    }
  }
}

esp_err_t MQTTdispatcher::initialize(esp_mqtt_client_config_t *config) {
  // ESP_LOGI(TAG,"in dispatcher init");
  // esp_err_t err;
  // sets the standard device id
  strcpy(myMqttID, ED_sysstd::ESP_std::mqttName());
  _mqtt =
      MqttClient::create(config); // creates the client with the default config
  if (_mqtt == nullptr) {
    ESP_LOGE(TAG, "Failed to initialize MQTT client");
    return ESP_FAIL;
  }
  clHandle = _mqtt->getHandle();
  // registers the callback to execute when mqqt connects
  _mqtt->registerConnectedCallback(onMqttConnected);
  _mqtt->registerDataCallback(onMqttData);

  T_info_loop = xTimerCreate("Info loop timer",   // Timer name
                             pdMS_TO_TICKS(3000), // Period in ticks (1000 ms)
                             pdTRUE,    // Auto-reload (pdFALSE for one-shot)
                             (void *)0, // Timer ID (optional)
                             T_info_timer_callback // Callback function
  );

  if (T_info_loop == nullptr) {
    ESP_LOGE(TAG, "Could not initialize timer for the info loop");
    return ESP_FAIL;
  }
  xTaskCreate(infoPublisherTask, "InfoPublisher", 4096, nullptr, 5,
              &infoPublisherTaskHandle);
  xTimerStart(T_info_loop, 0); // Start with no block time
  return ESP_OK;
}

std::string MQTTdispatcher::stdPingJson() {
  ED_JSON::JsonEncoder encoder;
  encoder.add("StationID", ED_sysstd::ESP_std::mqttName());
  encoder.add("NetID", ED_sysstd::ESP_std::NetwName());
  encoder.add("MAC", ED_sysstd::ESP_std::stdMAC());
  encoder.add("IP", ED_sysstd::ESP_std::curIP());
  encoder.add("time", ED_sysstd::ESP_std::curStdTime());
  encoder.add("upTime", ED_sysstd::ESP_std::upTime());
  encoder.add("location", "<location>>");
  encoder.add("Project", "<project>");

  return encoder.getJson();
};

void MQTTdispatcher::T_info_timer_callback(TimerHandle_t handle) {

  xTaskNotifyGive(infoPublisherTaskHandle);
}

void MQTTdispatcher::infoPublisherTask(void *arg) {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    publishInfo();
  }
}

void MQTTdispatcher::publishInfo() {
  // ESP_LOGI(TAG, "Timer callback executed!");
  if (_mqtt == nullptr)
    ESP_LOGI(TAG, "_mqtt nullptr!\n");
  // esp_mqtt_client_handle_t handleqtt = _mqtt->getHandle();
  // ESP_LOGI(TAG, "gethandle %s!\n", (handleqtt == nullptr) ? "nullptr" :
  // "OK");

  esp_mqtt_client_publish(
      clHandle, ("info/connection/" + std::string(myMqttID)).c_str(),
      stdPingJson().c_str(), 0, MqttClient::MqttQoS::QOS1, true);
  // ESP_LOGI(TAG, "END Timer callback executed!\n");
};

} // namespace ED_MQTT_dispatcher