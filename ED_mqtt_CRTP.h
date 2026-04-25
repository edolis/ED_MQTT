#pragma once
#include <esp_event_base.h>
#include <memory>
#include <mqtt_client.h>
#include "esp_log.h"

namespace ED_MQTT {

/**
 * @brief
 *CRTP version. using templates instead of plain inheritance.
 Not really performing better and supporting cleaner coding, so stick to the ED_mqtt non CRTP versions
 * note! the mosquitto client should connect encrypting the transmitted
credentials using TLS
 * which requires a certificate is loaded on the client side.
 * the certificate ca.crt should be saved in a certs subfolder of the project
and loaded in the firmware by adding to the platformio.ini

 board_build.embed_txtfiles = certs/ca.crt
 *

 ANNOTATION ON ENVIRONMENT SETUP.
 Two options - use the predefined template  ED_templ_ESP

 OR

 configure manually as specidfied below:

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
d) to optimize the size, disable unused TLS components in
sdkconfig.<board>.default # Disable unused cryptographic algorithms - keeps oly
the ones used for my sha256WithRSAEncryption and TLSv1.2 CONFIG_MBEDTLS_ECP_C=n
CONFIG_MBEDTLS_ECDSA_C=n
CONFIG_MBEDTLS_SHA512_C=n
CONFIG_MBEDTLS_SHA384_C=n
CONFIG_MBEDTLS_AES256_C=n

# Disable PEM and X.509 writing (only parsing needed)
CONFIG_MBEDTLS_PEM_PARSE_C=n
CONFIG_MBEDTLS_PEM_WRITE_C=n
CONFIG_MBEDTLS_X509_CREATE_C=n
CONFIG_MBEDTLS_X509_WRITE_C=n

# Disable debugging and test features
CONFIG_MBEDTLS_DEBUG=n
CONFIG_MBEDTLS_TEST_NULL_ENTROPY=n

# Disable session resumption and client-side TLS (if ESP32 is only a client)
CONFIG_MBEDTLS_SSL_SESSION_TICKETS=n
CONFIG_MBEDTLS_SSL_CLI_C=n

 */
//ESP_EVENT_DECLARE_BASE(ED_MQTT_SENSOR_EVENTS);

// Define the event IDs for this base
//enum { ED_MQTT_SENSOR_EVENT_DATA_READY, ED_MQTT_SENSOR_EVENT_ERROR };

// MqttClient_CRTP.h

extern const char* mqtt_event_names[] ;


   /*NOTE
   * same size, header messy, TAG management forced to be inclass
   * on derived classes core code copied... consider better the not CRTP
   * versions defined in ED_mqtt.h
   */

template<typename Derived>
class MqttClient_CRTP {
protected:
  constexpr static const char* TAG = "MqttClient_CRTP"; //tag must be defined inside the template. don't use constexpr in the header! redefinition risk
  private:
bool eventsRegistered=false;
//single instance. Notice! derived class must redeclare to store thier single instance and hide the base one.
  static inline std::unique_ptr<Derived> _instance=nullptr;

public:
  /**
   * @brief initializes a singleton which creates and manages a single
   * MQTT connection and implement core
   * handling capabilities. handling can be expanded in derived classes
   * @param config the configuration for the broker
     */
  static Derived* create(esp_mqtt_client_config_t config)
  {
  auto instance = std::make_unique<Derived>();
  esp_err_t err = instance->start(config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    return nullptr;
  }
  _instance = std::move(instance);
  return _instance.get();
};

  static Derived* getInstance() {
    return _instance.get();
  };

  /// Mosquitto: quality of service
  class MqttQoS {
    public:
    enum Value : int {
      QOS0 = 0, // At most once
      QOS1 = 1, // At least once
      QOS2 = 2  // Exactly once
    };
  };

  virtual ~MqttClient_CRTP() {
  if (eventsRegistered && client != nullptr) {
    esp_mqtt_client_unregister_event(client, MQTT_EVENT_ANY, mqtt_event_trampoline);
    eventsRegistered = false;
  }
  if (client != nullptr) {
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    client = nullptr;
  }
};

  private:

  // static trampoline callback method to redirect to the instance override-able
// handler
static void mqtt_event_trampoline(void* handler_args,
                                  esp_event_base_t base,
                                  int32_t event_id,
                                  void* event_data) {
  auto* self = static_cast<Derived*>(handler_args);
  self->handleEvent(base, event_id, event_data);
}

  protected:


virtual void handleEvent(esp_event_base_t base, int32_t event_id, void* event_data) {
  auto* event = static_cast<esp_mqtt_event_handle_t>(event_data);
  switch (event_id) {
    case MQTT_EVENT_CONNECTED:
      ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
      esp_mqtt_client_subscribe(client, "/test/topic", 0);
      esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32", 0,
                              MqttQoS::QOS1, 0);
      break;
    case MQTT_EVENT_DISCONNECTED:
      ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED — attempting reconnect...");
      break;
    case MQTT_EVENT_ERROR:
      ESP_LOGE(TAG, "MQTT_EVENT_ERROR — transport error");
      break;
    case MQTT_EVENT_DATA:
      ESP_LOGI(TAG, "MQTT_EVENT_DATA");
      ESP_LOGI(TAG, "data received with TOPIC=%s  DATA=%s", event->topic, event->data);
      break;
    default:
      ESP_LOGI(TAG, "MQTT event id:%s", mqtt_event_names[event->event_id]);
      break;
  }
}


  /**
   * @brief registers the event callback and launches the broker.
   * @param config
   * @return esp_err_t
   */
   esp_err_t start(esp_mqtt_client_config_t config){
  // ... setup and configuration code for the MQTT client
  // notice just one shared client will be created in the base class
  // in order to optimize resources.
  // multiple client configuration could also be used , to manage different QoS
  // etc. but that would consume additional resources and increase complexity
  esp_err_t err;
  // TODO need initialize clientID with controlled value
  if (client == nullptr) { //
    // can initialize just once. Since multiple derived classes and/or the base
    // classes share the same mqtt client, they just need to register their
    // event handlers
    client = esp_mqtt_client_init(&config);
    if (client == nullptr)
      return ESP_FAIL;
    err = esp_mqtt_client_start(client);
    if (err != ESP_OK)
      return err;
  }

  // ESP_LOGI(TAG,"registering handler");
  if (!eventsRegistered) {
    err = esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
                                         mqtt_event_trampoline, this);
    eventsRegistered = true;
    return err;
  }
  return ESP_OK;
};
  // Public method to register the handler.
  // Derived classes can call this to link their instance to the event loop.
  // void registerHandler(esp_mqtt_client_handle_t client);
  esp_mqtt_client_handle_t client =       nullptr; // not static as multiple derived classes might be used
               // simultaneously

};

/**
 * @brief example of derived class.
 * derived class basically overrides handler to handle additional events
 * which can be custom defined extending the
 *
 */

class SAMPLE_derivedMqttClient_CRTP : public MqttClient_CRTP<SAMPLE_derivedMqttClient_CRTP> {
friend class MqttClient_CRTP<SAMPLE_derivedMqttClient_CRTP>;

  private:
bool eventsRegistered=false;

//single instance. Notice! derived class must redeclare to store thier single instance and hide the base one.
  static inline std::unique_ptr<SAMPLE_derivedMqttClient_CRTP> _instance=nullptr; //REQUIRED TO SHADOW BASE

public:
  // static SAMPLE_derivedMqttClient_CRTP* create(esp_mqtt_client_config_t config);//REQUIRED TO SHADOW BASE
   void send_ping_message(); //TODO temporary for testing - implement as bound to connect event launching timer led ping with reported info on the status of the ESP

  // static SAMPLE_derivedMqttClient_CRTP* getInstance() {
  //   ESP_LOGI(TAG,"in get instsance");
  //   return _instance.get();
  // };
protected:
  // This is the implementation of the pure virtual function from the base
  // class. You can't make this private because it needs to be accessible to the
  // base class's static handler, but you can make it protected to prevent
  // outside access.
  void handleEvent(esp_event_base_t base, int32_t event_id,
                   void *event_data) override
                    {
  // these override the default event handler of the base class
  auto *event = static_cast<esp_mqtt_event_t *>(event_data);
  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    esp_mqtt_client_subscribe(client, "/test/topic", 0);
    esp_mqtt_client_publish(client, "/ESP_HANDSHAKE", "Hello from ESP32", 0,
                            MqttQoS::QOS1, 0);
    break;
  case MQTT_EVENT_DISCONNECTED:
    ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED — attempting reconnect...");
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT_EVENT_ERROR — transport error");
    break;
  case MQTT_USER_EVENT:
    ESP_LOGE(TAG,
             "MQTT_USER EVENT"); // this is the place where discriminate payload
    break;
  case MQTT_EVENT_DATA:
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    printf("TOPIC=%.*s\n", event->topic_len, event->topic);
    printf("DATA=%.*s\n", event->data_len, event->data);
    break;
  default:
    MqttClient_CRTP::handleEvent(base, event_id, event_data);
    break;
  };
};;
  // static std::unique_ptr<MqttClient_CRTP> makeInstance();

private:
  // You can have private members specific to this client
  int messageCounter = 0;
};

} // namespace ED_MQTT