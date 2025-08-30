#pragma once
#include <esp_event_base.h>
#include <memory>
#include <mqtt_client.h>
#include <functional>

namespace ED_MQTT {

/**
 * @brief note! the mosquitto client should connect encrypting the transmitted
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
ESP_EVENT_DECLARE_BASE(ED_MQTT_SENSOR_EVENTS);

// Define the event IDs for this base
enum { ED_MQTT_SENSOR_EVENT_DATA_READY, ED_MQTT_SENSOR_EVENT_ERROR };

// MqttClient.h
// Embedded CA certificate
extern const uint8_t ca_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t ca_crt_end[] asm("_binary_ca_crt_end");

extern const char* mqtt_event_names[] ;
/// callback function type to allow subscribers perform action at mqtt connection
using MqttConnectedCallback = std::function<void(esp_mqtt_client_handle_t)>;

class MqttClient {
private:
static void setDefaultConfig();
static inline std::vector<MqttConnectedCallback> connected_callbacks;
bool eventsRegistered=false;
//single instance. Notice! derived class must redeclare to store thier single instance and hide the base one.
  static inline std::unique_ptr<MqttClient> _instance=nullptr;
/// @brief default configuration for the connection to the mosquitto broker
static const   esp_mqtt_client_config_t mqtt_default_cfg ;
public:
void registerConnectedCallback(MqttConnectedCallback callback) {
        connected_callbacks.push_back(callback);
    }
    /**
     * @brief initializes a singleton which creates and manages a single
     * MQTT connection and implement core
     * handling capabilities. handling can be expanded in derived classes
     * @param config the configuration for the broker
     * @return MqttClient* pointer to the singleton instance
     */
    static MqttClient *create(esp_mqtt_client_config_t *config = nullptr);
    /**
     * @brief сreates the Mosquitto client with the standard configuration
     *
     * @return MqttClient* pointer to the singleton instance
     */
    // static MqttClient* create();

    static MqttClient *getInstance()
    {
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
  virtual ~MqttClient() ;

  private:

  static inline esp_mqtt_client_config_t mqttConfig={}; //internal copy of the current mqtt configuration

  /// @brief trampoline function which dispatched the static callback of the base class to the instance callback of the most derived class
  static void mqtt_event_trampoline(void *handler_args,
                                  esp_event_base_t base,
                                  int32_t event_id,
                                  void *event_data);


  protected:
  /**
   * @brief registers the event callback and launches the broker.
   * @param config
   * @return esp_err_t
   */
   esp_err_t start(esp_mqtt_client_config_t config);
  // Public method to register the handler.
  // Derived classes can call this to link their instance to the event loop.
  // void registerHandler(esp_mqtt_client_handle_t client);
  esp_mqtt_client_handle_t client =       nullptr; // not static as multiple derived classes might be used
               // simultaneously

  // This is the private static member function that acts as the C callback.
  // It's static to remove the implicit 'this' pointer.
  // static void handleEventCallback(void *handler_args, esp_event_base_t base,
  //                                 int32_t event_id, void *event_data);
  /**
   * @brief provides a base implementation for the core MQTT events.
   * Derived classes can extend or partially override by processing selected
   * events and calling base class to access default im,plementation
   *
   * @param base
   * @param event_id
   * @param event_data
   */
  virtual void handleEvent(esp_event_base_t base, int32_t event_id,
                           void *event_data);
};

/**
 * @brief example of derived class.
 * derived class basically overrides handler to handle additional events
 * which can be custom defined extending the
 *
 */

class SAMPLE_derivedMqttClient : public MqttClient {
  private:
bool eventsRegistered=false;

//single instance. Notice! derived class must redeclare to store thier single instance and hide the base one.
  static inline std::unique_ptr<SAMPLE_derivedMqttClient> _instance=nullptr; //REQUIRED TO SHADOW BASE

public:
  static esp_err_t create(esp_mqtt_client_config_t config);//REQUIRED TO SHADOW BASE
   void send_ping_message(); //TODO temporary for testing - implement as bound to connect event launching timer led ping with reported info on the status of the ESP

  static SAMPLE_derivedMqttClient* getInstance() {
    return _instance.get();
  };
protected:
  // This is the implementation of the pure virtual function from the base
  // class. You can't make this private because it needs to be accessible to the
  // base class's static handler, but you can make it protected to prevent
  // outside access.
  void handleEvent(esp_event_base_t base, int32_t event_id,
                   void *event_data) override;
  static std::unique_ptr<MqttClient> makeInstance();

private:
  // You can have private members specific to this client
  int messageCounter = 0;
};
/*
MyMqttHandler* handler_instance = new MyMqttHandler();

esp_event_handler_register(MQTT_EVENT, ESP_EVENT_ANY_ID,
                           &MqttHandlerBase::mqtt_event_handler,
                           handler_instance);

    }
*/
/*
class MQTTcomm {
private:
  static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                 int32_t event_id, void *event_data);
  static inline esp_mqtt_client_handle_t client = nullptr;

public:
  static void send_ping_message();
  static esp_err_t start(esp_mqtt_client_config_t config);
};
*/
} // namespace ED_MQTT