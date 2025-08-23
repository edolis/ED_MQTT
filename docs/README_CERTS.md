# storage and loading of certificates for TLS


## Context
the certificate is used to identify the server during the exchange of encrypted credentials during connection with TSL protocol.
This encryption is the one used by clients like MQTT (Wifi uses its own encryption).

The challenges are:

a. storing the certificate on the ESP so that it can be changed without need to recompile the library using it
b. using properly the certificate, recognizing the server using a DN [instead of the IP]
c. ensuring that the mqtt sends the appropriate response certificate at ESP request during the handshake

### A ] Storing the certificate

1. in the project, save your (self=signed) certificate in src\certs. thin will be the folder used to store sensitive data which should not be included in the {~public?} GIT repositories
2. specify the file must be included by the compiler in the firmware:
   - add to platformio the embed request ``` board_build.embed_txtfiles = src/certs/ca.crt  ```
   - add to the cMakesFiles of the src folder the EMBED_TXTFILES directive as follows
    ```
        idf_component_register(SRCS ${app_sources}
                            INCLUDE_DIRS "."
                            EMBED_TXTFILES certs/ca.crt
                            )
                                ```
3.  reference the file contents using
```
extern const uint8_t mycert_crt_start[] asm("_binary_ca_crt_start");
extern const uint8_t mycert_crt_end[] asm("_binary_ca_crt_end");
```
Notice that the asm ids are generated from the name of the file *without reference to the file path*.

### B} using the certificate,
To properly use the certificate on the ESP, you need to configures so that it can match the CN of the certificate.
your raspberry must be able to resolve the host name stored in the certificate as DN {this is a good practice required to allow the change of the server IP freely without breaking the communication with the devices in the network!}. In the setup where the MQTT is running on a raspberry pi you need to
- set up raspberry to resolve its CN
  1. run and install dnsmasq which will resolve names
  2. require that the the raspberry resolves its own host name adding to the ```/etc/dnsmasq.conf``` the following line
  ```
  #serve dhcp address
  dhcp-option=6,192.168.1.220
  ```
  1. test your ESP resolves the hostname. you can use ```DNSlookup``` in ED_SYS library, it should return the IP of the raspberry instead of 0.0.0.0
- configure the MQTT server to send the right certificate to the client at handshake:
  - in mosquitto.cong do **not** declare the line specifying the ca.crt as it will be sent to the client together with the server certificate which will break the validation from esp.
You can test using ```openssl x509 -in /etc/mosquitto/certs/server.crt -text -noout```, which should give just the ca.crt of the server
- connect to the MQTT specifying the right credentials (username, password) and the correct server
```
esp_mqtt_client_config_t mqtt_cfg = {
      .broker =
          {
              .address =
                  {
                      .uri = "mqtts://raspi00:8883",
                      //.hostname = "raspi00",
                  },
              .verification =
                  {
                      .use_global_ca_store = false,
                      .certificate =
                          reinterpret_cast<const char *>(ca_crt_start),
                      .certificate_len =
                          //static_cast<size_t>(ca_crt_end - ca_crt_start),
                         static_cast<size_t>( reinterpret_cast<uintptr_t>(ca_crt_end) -
                         reinterpret_cast<uintptr_t>(ca_crt_start)), // to avoid warning in code analysis, conversion to ram memory pointers.

                      .skip_cert_common_name_check = false,
                  },
          },
      .credentials =
          {
              .username = "username",
              .authentication =
                  {
                      .password = "password",
                  },
          },
  };
```
In theory you could use the IP in the URI and host for the matching of the CN, but I could not make it work.
