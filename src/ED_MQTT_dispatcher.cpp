#include "ED_MQTT_dispatcher.h"
#include "ED_json.h"
#include <esp_log.h>
// #include <regex> do NOT use C++ regex with ESP32
#include <regex.h>
#include "ED_sys.h"
#include <map>


namespace ED_MQTT_dispatcher {


//#region CommandRegistry

    void CommandRegistry::registerCommand(const ctrlCommand& cmd) {


        registry.emplace(cmd.cmdID, cmd);
        // registry[cmd.cmdID] = cmd; //this does not work unless you define a constructor for empty object
    }
 ctrlCommand* CommandRegistry::getCommand(const std::string commandID) {
        auto it = registry.find(commandID);
    if (it != registry.end()) {
        return &it->second;  // Return pointer to the found ctrlCommand
    }
    return nullptr;  // Not found
    };

    bool CommandRegistry::dispatch(const std::string& cmdID) {
        //NOTE needs removing?
      ESP_LOGI("DISPATCH","in dispatch step 2");
      auto it = registry.find(cmdID);
      if (it != registry.end()) {
          ESP_LOGI("DISPATCH","in dispatch step 3");
            ctrlCommand& cmd = it->second;
            if (cmd.funcPointer) {
                cmd.funcPointer(&cmd);
                return true;
            }
        }
        return false;
    }

    std::string CommandRegistry::getHelp() const {
        std::string help;
        for (const auto& [id, cmd] : registry) {
            help += ctrlCommand::toHelpString(cmd) + "\n";
        }
        return help;
    }

    std::map<std::string, ctrlCommand>& CommandRegistry::getRegistry() {
        return registry;
    }

//#endregion CommandRegistry

    void CommandWithRegistry::registerCommand(const ctrlCommand& cmd) {

registry.registerCommand(cmd);
        // registry.emplace(cmd.cmdID, cmd);
        // registry[cmd.cmdID] = cmd; //this does not work unless you define a constructor for empty object
    }





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
    static char topic_conn[64];
    static char topic_info[64];
    static bool built = false;
    if (!built) {
      snprintf(topic_conn, sizeof topic_conn, "devices/connection/%s", myMqttID);
      snprintf(topic_info, sizeof topic_info, "info/connection/%s", myMqttID);
      built = true;
    }

    char msg[96];
    int n = snprintf(msg, sizeof msg, "%s connects.", myMqttID);
    if (n < 0) n = 0;
    if (n > (int)sizeof msg) n = sizeof msg;

    esp_mqtt_client_publish(client, topic_conn, msg, n,
                            MqttClient::MqttQoS::QOS1, true);

    esp_mqtt_client_subscribe(client, "cmd", 0);

    // Consider prebuilding once and reusing the JSON (see §3)
    std::string info = stdPingJson();
    esp_mqtt_client_publish(client, topic_info, info.c_str(), info.size(),
                            MqttClient::MqttQoS::QOS1, true);
  };

MqttDataCallback MQTTdispatcher::onMqttData =
    [](esp_mqtt_client_handle_t client,const char* topic, int topicLen, std::string& data, const int64_t msgID) {
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
          sub->grabCommand(commandID, payload,msgID);
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
          handleCommandObject(item,msgID);
        }
      } else if (unwrapped.isValidJson()) {
        handleCommandObject(unwrapped,msgID);
      } else {
        ESP_LOGW(TAG, "Malformed or unsupported payload: [%s]", data.c_str());
      }
      // publish back as test
      // esp_mqtt_client_publish(
      //     clHandle, ("info/ESPreceived/" + std::string(myMqttID)).c_str(),
      //     ("sending back payload: " + data).c_str(), 0,
      //     MqttClient::MqttQoS::QOS1, false);
    };

bool MQTTdispatcher::parseCommand(const std::string& input,
                                  std::string& commandID,
                                  std::string& payload) {
  // Format: :<CMD>[whitespace]<payload...>
  if (input.empty() || input[0] != ':') return false;

  size_t i = 1;
  // skip spaces after ':'
  while (i < input.size() && isspace((unsigned char)input[i])) ++i;
  if (i >= input.size()) return false;

  // commandID: until next space
  size_t start = i;
  while (i < input.size() && !isspace((unsigned char)input[i])) ++i;
  commandID.assign(input.data() + start, i - start);
  // uppercase in-place
  for (char& c : commandID) c = (char)std::toupper((unsigned char)c);

  // skip spaces before payload
  while (i < input.size() && isspace((unsigned char)input[i])) ++i;
  if (i < input.size()) payload.assign(input.data() + i, input.size() - i);
  else payload.clear();

  return true;
}

 void MQTTdispatcher::ackCommand(int64_t reqMsgID, std::string commandID,
                                ackType ackResult, std::string payload) {
  // build topic once (static lifetime)
  static char topic_ack[64];
  static bool built = false;
  if (!built) {
    snprintf(topic_ack, sizeof topic_ack, "ack/%s", myMqttID);
    built = true;
  }

  char ackbuf[256];
  const char* ack = (ackResult == ackType::OK) ? "OK" : "FAIL";
  int n = snprintf(ackbuf, sizeof ackbuf, "[%s] %s\n%.*s",
                   commandID.c_str(), ack,
                   (int)payload.size(), payload.c_str());
  if (n < 0) n = 0;
  if (n > (int)sizeof ackbuf) n = sizeof ackbuf;

  int rc = esp_mqtt_client_publish(clHandle, topic_ack, ackbuf, n,
                                   MqttClient::MqttQoS::QOS1, false);
  ESP_LOGI(TAG, "ackCommand publish rc=%d handle=%p", rc, (void*)clHandle);
}



void MQTTdispatcher::handleCommandObject(const ED_JSON::JsonEncoder &obj, int64_t cmdID) {
  // ESP_LOGI(TAG,"Step_2 isvalidjson %d",obj.isValidJson());
  if (!obj.isValidJson())
    return;
  // ESP_LOGI(TAG,"Step_3");
  auto commandOpt = obj.getString("cmd");
  auto dataOpt = obj.getString("data");

  if (commandOpt && dataOpt) {
    for (auto *sub : cmdSubscribers) {
      // ESP_LOGI(TAG, "calling");
      sub->grabCommand(*commandOpt, *dataOpt,cmdID);
    }
  }
}

esp_err_t MQTTdispatcher::initialize(esp_mqtt_client_config_t *config) {
  // ESP_LOGI(TAG,"in dispatcher init");
  // esp_err_t err;
  // sets the standard device id
  strcpy(myMqttID, ED_SYS::ESP_std::Device::mqttName());
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
  encoder.add("StationID", ED_SYS::ESP_std::Device::mqttName());
  encoder.add("NetID", ED_SYS::ESP_std::Device::netwName());
  encoder.add("MAC", ED_SYS::ESP_std::Device::stdMAC());
  encoder.add("IP", ED_SYS::ESP_std::Device::curIP());
  encoder.add("time", ED_SYS::ESP_std::Runtime::curStdTime());
  encoder.add("upTime", ED_SYS::ESP_std::Runtime::uptime());
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
  static char topic_info[64];
  static bool built = false;
  if (!built) {
    snprintf(topic_info, sizeof topic_info, "info/connection/%s", myMqttID);
    built = true;
  }

  // Option A: fixed buffer, zero dynamic allocs
  char buf[512];
  int n = snprintf(buf, sizeof buf,
    "{\"StationID\":\"%s\",\"NetID\":\"%s\",\"MAC\":\"%s\",\"IP\":\"%s\","
    "\"time\":\"%s\",\"upTime\":\"%s\",\"location\":\"%s\",\"Project\":\"%s\"}",
    ED_SYS::ESP_std::Device::mqttName(),
    ED_SYS::ESP_std::Device::netwName(),
    ED_SYS::ESP_std::Device::stdMAC(),
    ED_SYS::ESP_std::Device::curIP(),
    ED_SYS::ESP_std::Runtime::curStdTime(),
    ED_SYS::ESP_std::Runtime::uptime(),
    "<location>>",
    "<project>");
  if (n < 0) {n = 0;};

  if (n > (int)sizeof (buf)) n = sizeof buf;

  esp_mqtt_client_publish(clHandle, topic_info, buf, n,
                          MqttClient::MqttQoS::QOS1, true);
}


void CommandWithRegistry::grabCommand(const std::string commandID,
                                      const std::string commandData,
                                      int64_t msgID) {
  // receives a command ID and a string with the params,
  //  matches the registered commands and updates the parameters
  // and finally executes them
  //  const char* input = "defaultvalue -p1 value -p2 -p3 othervalue";
  const char* s = commandData.c_str();

  auto* cmd = registry.getCommand(commandID);
  if (!cmd) return;

  // _msgID without allocating a new string object
  char idbuf[24];
  snprintf(idbuf, sizeof idbuf, "%lld", (long long)msgID);
  cmd->optParam["_msgID"] = idbuf;

  // default value: first non-space token
  const char* p = s;
  while (*p && isspace((unsigned char)*p)) ++p;
  const char* d0 = p;
  while (*p && !isspace((unsigned char)*p)) ++p;
  if (p > d0) cmd->optParam["_default"] = std::string(d0, (size_t)(p - d0));

  // flags: -name [value]
  while (*p) {
    while (*p && isspace((unsigned char)*p)) ++p;
    if (*p != '-') break;
    ++p;

    const char* f0 = p;
    while (*p && isalnum((unsigned char)*p)) ++p;
    std::string flag(f0, (size_t)(p - f0));

    while (*p && isspace((unsigned char)*p)) ++p;
    std::string value;
    if (*p && *p != '-') {
      const char* v0 = p;
      while (*p && !isspace((unsigned char)*p)) ++p;
      value.assign(v0, (size_t)(p - v0));
    }
    cmd->optParam[flag] = value;
  }

  if (cmd->funcPointer) cmd->funcPointer(cmd);
};

} // namespace ED_MQTT_dispatcher