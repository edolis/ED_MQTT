idf_component_register(
    SRCS "ED_mqtt.cpp"  "ED_MQTT_dispatcher.cpp"
    INCLUDE_DIRS "." "$ENV{ESP_HEADERS}"
    REQUIRES
        mqtt
        esp_event
        log
        esp_timer
        freertos
        lwip
        esp_netif
        mbedtls
        diag
        ED_SYS
        ED_S_JSON
        ED_WIFI
)

# # Wrap every heap_caps_* call system-wide so the tracer intercepts ALL
# # allocations — including those from the MQTT5 library and lwIP.
# # INTERFACE propagates these flags to the final link step, not just this lib.
# # Remove these four lines if you want to disable the wrap interception.
# target_link_options(${COMPONENT_LIB} INTERFACE
#     -Wl,--wrap=heap_caps_malloc
#     -Wl,--wrap=heap_caps_calloc
#     -Wl,--wrap=heap_caps_realloc
#     -Wl,--wrap=heap_caps_free
# )