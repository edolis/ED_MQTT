# Summary: Migrating MQTT Client Hierarchy to CRTP

## Background

Throughout this conversation, we explored how to build a robust C++ MQTT client on ESP-IDF using a base class with virtual event handling and derived subclasses. We discussed factory methods (`create()`), singleton management via a static `_instance`, and the trampoline pattern to bridge C callbacks to C++ methods.

---

## Challenges in the Current Implementation

- Shadowing a single `_instance` in both base and derived classes led to confusion and incorrect storage of derived types.
- `std::unique_ptr` cannot be copied—must use `std::move`.
- Base `start()` lacked a final `return ESP_OK;` and risked multiple initializations.
- Registering event handlers twice without guarding could trigger duplicate callbacks.
- Redeclaring both `create()` and `_instance` for each derived type introduced boilerplate.

---

## Key Patterns Discussed

- **Trampoline Pattern**: Static C-style callback that casts `handler_args` to the correct instance and invokes `handleEvent()`.
- **Singleton via `unique_ptr`**: Factory method stores a single instance in a static member, with `getInstance()` to retrieve it.
- **CRTP (Curiously Recurring Template Pattern)**: Use a templated base class to eliminate per-derived boilerplate and produce correctly typed singletons.

---

## Migration Approach Using CRTP

1. **Define a CRTP Base Template**
   ```cpp
   template<typename Derived>
   class MqttClientCRTP {
   protected:
     static inline std::unique_ptr<Derived> _instance = nullptr;
     esp_mqtt_client_handle_t client = nullptr;
     bool handler_registered = false;

     static void mqtt_event_trampoline(void* a, esp_event_base_t b, int32_t e, void* d) {
       static_cast<Derived*>(a)->handleEvent(b, e, d);
     }

     esp_err_t start(const esp_mqtt_client_config_t& cfg) {
       if (!client) {
         client = esp_mqtt_client_init(&cfg);
         if (!client) return ESP_FAIL;
         auto err = esp_mqtt_client_start(client);
         if (err != ESP_OK) return err;
       }
       if (!handler_registered) {
         esp_mqtt_client_register_event(client, MQTT_EVENT_ANY,
           &MqttClientCRTP::mqtt_event_trampoline,
           static_cast<Derived*>(this));
         handler_registered = true;
       }
       return ESP_OK;
     }

   public:
     static Derived* create(const esp_mqtt_client_config_t& cfg) {
       if (!_instance) {
         auto inst = std::make_unique<Derived>();
         if (inst->start(cfg) != ESP_OK) return nullptr;
         _instance = std::move(inst);
       }
       return _instance.get();
     }

     static Derived* getInstance() {
       return _instance.get();
     }

     // Default no-op; override in Derived.
     void handleEvent(esp_event_base_t, int32_t, void*) {}
   };
   ```

2. **Declare a Derived Class**
   ```cpp
   class MyMqttClient : public MqttClientCRTP<MyMqttClient> {
     friend class MqttClientCRTP<MyMqttClient>;

   protected:
     void handleEvent(esp_event_base_t base, int32_t id, void* data) {
       // Custom logic here
     }

   public:
     void sendPing() {
       esp_mqtt_client_publish(client, "ping", "msg", 0, 1, 0);
     }
   };
   ```

---

## Benefits of the CRTP Approach

- Single definition of `create()`, `getInstance()`, `start()`, and trampoline logic.
- Correct return types (`Derived*`) without cast or boilerplate.
- Elimination of per-derived `_instance` and `create()` shadowing.
- Compile-time dispatch replaces virtual calls, reducing overhead.
- Each specialization gets its own static `_instance`.

---

## Next Steps

- Refactor existing base/derived code into the CRTP template.
- Remove now-unnecessary static members in non-template base.
- Add logging, error handling, or reconnect strategies into the CRTP `start()`.
- If run-time polymorphism is later required, introduce a minimal `IMqttClient` interface that the CRTP base can implement.