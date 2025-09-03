// #region StdManifest
/**
 * @file main_Json.cpp
 * @brief
 *
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version GIT_VERSION: v1.0.0.0-0-dirty
 * @tagged as: SNTP-core
 * @commit hash: g5d100c9 [5d100c9e7fbf8030cd9e50ec7db3b7b6333dbee1]
 * @build ID: P20250902-222045-5d100c9
 *  @compiledSizeInfo begin

    .iram0.text      85 720    .dram0.data  12 644
    .flash.text     980 982    .dram0.bss   25 272
    .flash.appdesc      256    ―――――――――――――――――――
    .flash.rodata   151 384    total        37 916
    ―――――――――――――――――――――――                       
    subtotal        1 218 342                       

    @compiledSizeInfo end
 * @date 2025-08-28
 */

static const char *TAG = "ESP_main_loop";

// #region BuildInfo
namespace ED_SYSINFO {
// compile time GIT status
struct GIT_fwInfo {
  static constexpr const char *GIT_VERSION = "v1.0.0.0-0-dirty";
  static constexpr const char *GIT_TAG = "SNTP-core";
  static constexpr const char *GIT_HASH = "g5d100c9";
  static constexpr const char *FULL_HASH =
      "5d100c9e7fbf8030cd9e50ec7db3b7b6333dbee1";
  static constexpr const char *BUILD_ID = "P20250828-122422-5d100c9";
};
} // namespace ED_SYSINFO
// #endregion
// #endregion

#include "ED_JSON.h"
#include "ED_sysInfo.h"
#include "ED_sysstd.h"
#include "ED_wifi.h"
#include "ED_mqtt.h"
#include "ED_MQTT_dispatcher.h"
#include <string.h>

#ifdef DEBUG_BUILD
#endif
#include <map>
#include <esp_log.h>

using namespace ED_JSON;
using namespace ED_SYSINFO;
// using namespace ED_MQTT_dispatcher;

class TestCmdReceiver: public ED_MQTT_dispatcher::iCommand{

public:
static ED_MQTT_dispatcher::CommandRegistry registry;
virtual void grabCommand(const std::string commandID, const std::string commandData) override
{
ESP_LOGI("TestCmdReceiver", "received cmd [%s] data [%s]", commandID.c_str(), commandData.c_str());
ED_MQTT_dispatcher::ctrlCommand* candidate = nullptr;
auto& regMap = registry.getRegistry();
auto it = regMap.find(commandID);
if (it != regMap.end()) {
  ESP_LOGI(TAG,"element found");
    candidate = &it->second;
    std::unordered_map<std::string, std::string> parsedParams = JsonEncoder::parseJsonToMap(commandData);
    ED_MQTT_dispatcher::ctrlCommand::overrideParams(*candidate, parsedParams);
    registry.dispatch(commandID);
}
};

static void grabCommand(ED_MQTT_dispatcher::ctrlCommand * ctrcomd){

ESP_LOGI("TestCmdReceive>GrabCommandr", "executing cmd [%s] cmd Help [%s]", ctrcomd->cmdID.c_str(), ED_MQTT_dispatcher::ctrlCommand::toHelpString(*ctrcomd).c_str());

};

static void init(){

  ED_MQTT_dispatcher::ctrlCommand sdpiCmd;
sdpiCmd.cmdID = "SDPI";
sdpiCmd.cmdDex = "Set data polling interval";
sdpiCmd.scope = ED_MQTT_dispatcher::ctrlCommand::cmdScope::GLOBAL;
sdpiCmd.optParam["interval"] = "Polling interval in seconds";
sdpiCmd.funcPointer = static_cast<void(*)(ED_MQTT_dispatcher::ctrlCommand*)>(&TestCmdReceiver::grabCommand);;
ESP_LOGI(TAG,"Step_ funcpointer is null? %d",sdpiCmd.funcPointer==nullptr);

  registry.registerCommand(sdpiCmd);
};
};

ED_MQTT_dispatcher::CommandRegistry TestCmdReceiver::registry;


extern "C" void app_main(void) {

#ifdef DEBUG_BUILD

#endif
std::map<int, MacAddress> testMap;



TestCmdReceiver::init();


  char buffer[18] = "";

  ED_JSON::JsonEncoder encoder;
  ED_JSON::JsonEncoder encoderMAC;

  for (const auto &pair : ESP_MACstorage::getMacMap()) {
    esp_mac_type_t type = pair.first;
    const MacAddress &mac = pair.second;

    char buffer[18];
    std::string macStr = std::string(mac.toString(buffer, sizeof(buffer)));

    encoderMAC.add(std::string(esp_mac_type_str[type]), macStr);
  }
  encoder.add("deviceMACs", encoderMAC);
  encoder.add("intKey", 42);
  encoder.add("boolKey", true);
  encoder.add("nullKey", nullptr);
  encoder.add("arrayKey", std::vector<std::string>{"item1", "item2", "item3"});
static TestCmdReceiver crec;

ED_wifi::WiFiService::subscribeToIPReady([]() {
    ED_MQTT_dispatcher::MQTTdispatcher::initialize();

    ED_MQTT_dispatcher::MQTTdispatcher::subscribe(&crec);
  });
  ED_wifi::WiFiService::launch();

  // Optional hardware setup
  // gpio_set_direction(LED_BUILTIN, GPIO_MODE_OUTPUT);

#ifdef DEBUG_BUILD

#endif

  while (true) {
    // shows the results of the first calls which will happen when network not
    // initialized, afterwards calls will get froper feedback
  // uint8_t mockMac[6] = {0x98, 0x3D, 0xAE, 0x41, 0x2F, 0x6C};
  // // ESP_LOGI(TAG,"start test 1");
  // MacAddress m(mockMac);
  // // ESP_LOGI(TAG,"start test 2");
  // testMap[0] = m;
  // ESP_LOGI(TAG,"start test 3");

  // MacAddress mad=ESP_MACstorage::getMac(ESP_MAC_BASE);
  // char buffer[18]="";
  // ESP_LOGI(TAG,"start test %s", mad.toString(buffer,sizeof(buffer),':'));
  // ESP_LOGI(TAG,"stdmqttname %s", ED_sysstd::ESP_std::mqttName());

    vTaskDelay(3000 / portTICK_PERIOD_MS);
    // ESP_LOGI(TAG, "JSON Output: %s", encoder.getJson().c_str());
  }
}
