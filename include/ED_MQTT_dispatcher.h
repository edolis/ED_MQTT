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

using namespace ED_MQTT;

namespace ED_MQTT_dispatcher {


  // using MqttDataGrabber = std::function<void(,std::string,std::string)>;

class iCommand{

public:
virtual void grabCommand(const std::string commandID, const std::string commandData)=0;

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
  static  void subscribe(iCommand* subscriber) {
        cmdSubscribers.push_back(subscriber);
    }

private:
static std::vector<iCommand*> cmdSubscribers;
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