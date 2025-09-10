// #region StdManifest
/**
 * @file ED_MQTT_dispatcher.h
 * @brief Dispatcher class for ESP. tasked with enforcing communication
 * standards with the Mosquitto server running periodic tasks, decoding commands
 * ,
 *
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version 0.1
 * @date 2025-08-28
 */
// #endregion

#pragma once

#include "ED_mqtt.h"
#include "ED_sysstd.h"
#include "esp_timer.h"
#include <vector>
#include "secrets.h"
#include <string>
#include <map>

using namespace ED_MQTT;

namespace ED_MQTT_dispatcher {


  // using MqttDataGrabber = std::function<void(,std::string,std::string)>;

class iCommandRunner{

public:
virtual void grabCommand(const std::string commandID, const std::string commandData)=0;

};

/**
 * @brief defines a command callable from the user via MQTT
 * in order to modify the behaviour of the application
 */
struct ctrlCommand {
    enum class cmdScope {
        LOCALONLY,
        GLOBAL
    };

    std::string cmdID;
    std::string cmdDex; // description
    cmdScope scope;
    std::unordered_map<std::string, std::string> optParam;

     ctrlCommand(const std::string& id,
                const std::string& dex,
                cmdScope sc,
                const std::unordered_map<std::string, std::string>& params = {})
        : cmdID(id), cmdDex(dex), scope(sc), optParam(params) {}
    // Function pointer to handler
    std::function<void(ctrlCommand*)> funcPointer;

    // Help string generator
    static std::string toHelpString(const ctrlCommand& cmd) {
        std::string help = "Command: " + cmd.cmdID + "\n";
        help += std::string("Scope: ")  + (cmd.scope == cmdScope::GLOBAL ? "GLOBAL" : "LOCALONLY") + "\n";
        help += "Description: " + cmd.cmdDex + "\n";
        help += "Options:\n";
        for (const auto& [key, val] : cmd.optParam) {
            help += "  - " + key + ": " + val + "\n";
        }
        return help;
    }

    // Static method to override parameters from JSON
    static void overrideParams(ctrlCommand& cmd, const std::unordered_map<std::string, std::string>& jsonParams) {
        for (const auto& [key, val] : jsonParams) {
            cmd.optParam[key] = val;
        }
    }
};

class CommandRegistry {
public:
    void registerCommand(const ctrlCommand& cmd) {


        registry.emplace(cmd.cmdID, cmd);
        // registry[cmd.cmdID] = cmd; //this does not work unless you define a constructor for empty object
    }
 ctrlCommand* getCommand(const std::string commandID) {
        auto it = registry.find(commandID);
    if (it != registry.end()) {
        return &it->second;  // Return pointer to the found ctrlCommand
    }
    return nullptr;  // Not found
    };

    bool dispatch(const std::string& cmdID) {
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

    std::string getHelp() const {
        std::string help;
        for (const auto& [id, cmd] : registry) {
            help += ctrlCommand::toHelpString(cmd) + "\n";
        }
        return help;
    }

    std::map<std::string, ctrlCommand>& getRegistry() {
        return registry;
    }

private:
    std::map<std::string, ctrlCommand> registry;
};

class CommandWithRegistry : public iCommandRunner {
public:
    CommandRegistry registry;

virtual void grabCommand(const std::string commandID, const std::string commandData)override ;


    void registerCommand(const ctrlCommand& cmd) {
        registry.registerCommand(cmd);
    }

    bool dispatchCommand(const std::string& cmdID) {
        return registry.dispatch(cmdID);
    }

    std::string help() const {
        return registry.getHelp();
    }
};


/**
 * @brief singleton class taking care of communications between the MQTT server
 * and the ESP32
 *
 */
class MQTTdispatcher {

public:
static void handleCommandObject(const ED_JSON::JsonEncoder &obj);
/// intialized the dispatcher with the given mosquitto configuration
static esp_err_t initialize(esp_mqtt_client_config_t *config = nullptr);
/// intialized the dispatcher with the default mosquitto configuration
//   static esp_err_t initialize();
/// runs the dispatcher
static esp_err_t run();
/// subscribes a command handler to the dispatcher command received event
static  void subscribe(iCommandRunner* subscriber) {
  cmdSubscribers.push_back(subscriber);
}

private:
/**
 * @brief parses a text command received from MQTT
 * in the format
 * :COMID parameterstring
 * these commands are meant to be sent directly by the user by typing them as MQTT message.
 * The commands managed by the application, instead, are meant to be sent as a JSON array
 * and decoded by handleCommandObject
 * @param input the full raw content of the MQTT message, unparsed
 * @param commandID parsed command ID
 * @param payload parsed payload
 * @return true raw content was a command, and parsing was successful
 * @return false raw contents was not in a suitable format
 */
static bool parseCommand(const std::string &input, std::string &commandID, std::string &payload);
static std::vector<iCommandRunner*> cmdSubscribers;
static inline esp_mqtt_client_handle_t clHandle=nullptr;
static TaskHandle_t infoPublisherTaskHandle;

static inline char myMqttID[18]="";
static inline TimerHandle_t  T_info_loop=nullptr; // information loop - just one
static inline std::vector<TimerHandle_t > T_data_loop=std::vector<TimerHandle_t >(); //dynamically handled data loops
  MQTTdispatcher() = default;
  static inline  ED_MQTT::MqttClient* _mqtt=nullptr;

  static std::string stdPingJson();

  // Callback function for the info loop
static void T_info_timer_callback(TimerHandle_t handle);
static void infoPublisherTask(void *arg);
static void publishInfo();
//   std::unique_ptr<MyMqttClient> MQTTclient;

/// @brief  static setup of connection to the broker
static MqttConnectedCallback onMqttConnected;
static MqttDataCallback onMqttData;
};

} // namespace ED_MQTT_dispatcher