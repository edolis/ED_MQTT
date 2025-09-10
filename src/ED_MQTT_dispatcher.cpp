#include "ED_MQTT_dispatcher.h"
#include "ED_json.h"
#include <esp_log.h>
// #include <regex> do NOT use C++ regex with ESP32
#include <regex.h>

namespace ED_MQTT_dispatcher {

static const char *TAG = "ED_MQTT_dispatcher";
TaskHandle_t MQTTdispatcher::infoPublisherTaskHandle = nullptr;
std::vector<iCommandRunner *> MQTTdispatcher::cmdSubscribers = {};

std::string toupper(const std::string &input) {
  std::string result = input;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  return result;
}

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
      // ESP_LOGI(TAG, "DISPATCHER Received topic [%s] data [%s] datalength %d",
      //          topic.c_str(), data.c_str(), data.length());

      // for (size_t i = 0; i < data.length(); ++i) {
      //   printf("[%02X] ", static_cast<unsigned char>(data[i]));
      // }
      // printf("\n");
      // ESP_LOGI(TAG, "Raw payload: [%.*s]", data.length(), data.c_str());

      // checks if it is command line format
      std::string commandID;
      std::string payload;
      if (parseCommand(data, commandID, payload)) {
        if (commandID == "H" || commandID == "HELP" || commandID == "HLP") {
          ESP_LOGI(TAG, "Received request to spell implemented commands");
          // TODO incomplete
          return;
        }

        for (auto *sub : cmdSubscribers) {
          // ESP_LOGI(TAG, "calling");
          sub->grabCommand(commandID, payload);
        }
        return;
      }
      ED_JSON::JsonEncoder decoder(data);
      if (decoder.isValidJson())
        ESP_LOGI(TAG, "decoder creator turns %s into %s", data.c_str(),
                 decoder.getJson().c_str());
      // unpacks the array in case the client sends json as a json array
      auto unwrapped = decoder.unwrapNestedArray();
      ESP_LOGI(TAG, "unwrapper array?  %d", unwrapped.isArray());
      if (unwrapped.isArray()) {
        int size = unwrapped.getArraySize();
        ESP_LOGI(TAG, "array size %d", size);
        for (int i = 0; i < size; ++i) {
          auto item = unwrapped.getArrayItem(i);
          ESP_LOGI(TAG, "Calling with item %d :  %s", i,
                   item.getCompactJson().c_str());
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

bool MQTTdispatcher::parseCommand(const std::string &input,
                                  std::string &commandID,
                                  std::string &payload) {
  regex_t regex;
  regmatch_t matches[3]; // matches[0] = full match, [1] = commandID, [2] = payload

  // POSIX-compatible pattern:
  // ^: → starts with colon
  // ([^[:space:]]+) → commandID: first non-space token after colon
  // [[:space:]]* → optional whitespace
  // (.*) → payload: everything else (can be empty)
  const char *pattern = "^:([^[:space:]]+)[[:space:]]*(.*)$";

  if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
    ESP_LOGE(TAG, "Regex compilation failed");
    return false;
  }

  int result = regexec(&regex, input.c_str(), 3, matches, 0);
  regfree(&regex);

  if (result == 0 && matches[1].rm_so != -1) {
    // Extract commandID
    int len1 = matches[1].rm_eo - matches[1].rm_so;
    commandID = toupper(input.substr(matches[1].rm_so, len1));

    // Extract payload
    if (matches[2].rm_so != -1 && matches[2].rm_eo != -1) {
      int len2 = matches[2].rm_eo - matches[2].rm_so;
      payload = input.substr(matches[2].rm_so, len2);
    } else {
      payload.clear();
    }

    ESP_LOGI(TAG, "Parsed command: <%s> with payload: <%s>", commandID.c_str(), payload.c_str());
    return true;
  }

  return false;
}


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

void CommandWithRegistry::grabCommand(const std::string commandID,
                                      const std::string commandData) {
  // receives a command ID and a string with the params,
  //  matches the registered commands and updates the parameters
  // and finally executes them
  //  const char* input = "defaultvalue -p1 value -p2 -p3 othervalue";
  const char *input = commandData.c_str();

  ctrlCommand *cmd = nullptr;
  cmd = registry.getCommand(commandID);

  if (cmd == nullptr)
    return; // command not found as managed command
  // Step 1: extract defaultvalue
  regex_t regex;
  regmatch_t matches[2];
  regcomp(&regex, "^([^[:space:]]+)", REG_EXTENDED);
  if (regexec(&regex, input, 2, matches, 0) == 0) {
    char defaultValue[64];
    int len = matches[1].rm_eo - matches[1].rm_so;
    strncpy(defaultValue, input + matches[1].rm_so, len);
    defaultValue[len] = '\0';
    printf("Default value: %s\n", defaultValue);
    cmd->optParam["default"] = defaultValue;
  }
  regfree(&regex);

  // Step 2: extract flags and values
  regcomp(&regex, "-([[:alnum:]]+)([[:space:]]+([^[:space:]]+))?",
          REG_EXTENDED);
  const char *cursor = input;
  while (regexec(&regex, cursor, 4, matches, 0) == 0) {
    char flag[32], value[64] = "";
    int flagLen = matches[1].rm_eo - matches[1].rm_so;
    strncpy(flag, cursor + matches[1].rm_so, flagLen);
    flag[flagLen] = '\0';

    if (matches[3].rm_so != -1) {
      int valLen = matches[3].rm_eo - matches[3].rm_so;
      strncpy(value, cursor + matches[3].rm_so, valLen);
      value[valLen] = '\0';
    }

    // printf("Flag: -%s, Value: %s\n", flag, value);
    cmd->optParam[flag] = value;
    if (matches[0].rm_eo > 0)
      cursor += matches[0].rm_eo;
    else
      break; // Prevent infinite loop
  }

  // dispatches the command
  if (cmd->funcPointer) {
    cmd->funcPointer(cmd);
  }
};

} // namespace ED_MQTT_dispatcher