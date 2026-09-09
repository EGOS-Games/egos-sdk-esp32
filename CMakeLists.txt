# Sources and requirements are deliberately NOT wrapped in if(CONFIG_...).
#
# egos_ethernet.c and egos_led.c already guard their whole contents with
# #ifdef CONFIG_EGOS_*_ENABLED, so compiling them unconditionally produces an
# empty translation unit when the option is off. That costs nothing, keeps the
# file list independent of when CMake learns about sdkconfig, and means the
# build cannot end up compiling a caller's #ifdef branch while omitting the
# file that satisfies it - which is how the SDK previously failed to link:
#
#   undefined reference to `egos_ethernet_init'
#   undefined reference to `egos_led_set_state'

set(srcs
    "src/egos_sdk.c"
    "src/egos_nvs.c"
    "src/egos_wifi.c"
    "src/egos_mqtt.c"
    "src/egos_discovery.c"
    "src/egos_connection.c"
    "src/egos_ethernet.c"   # empty unless CONFIG_EGOS_ETHERNET_ENABLED
    "src/egos_led.c"        # empty unless CONFIG_EGOS_STATUS_LED_ENABLED
)

set(requires
    nvs_flash
    esp_wifi
    esp_event
    esp_timer
    json
    mdns
    mqtt
    esp_netif
    esp_eth             # for src/egos_ethernet.c
    esp_driver_ledc     # for src/egos_led.c
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES ${requires}
)
