set(APP_ROLE "standalone" CACHE STRING "Firmware role: standalone, device, or receiver")
set_property(CACHE APP_ROLE PROPERTY STRINGS standalone device receiver)
set(COMMON_COMPONENTS
    spi_flash
    esp_adc
    esp_event
    esp_netif
    esp_wifi
    nvs_flash
    esp_timer
    esp_driver_gpio
    esp_driver_usb_serial_jtag
    vfs)

if(APP_ROLE STREQUAL "standalone")
    idf_component_register(SRCS "hello_world_main.c"
                           PRIV_REQUIRES ${COMMON_COMPONENTS}
                           INCLUDE_DIRS ".")
elseif(APP_ROLE STREQUAL "device")
    idf_component_register(SRCS "device_main.c" "espnow_link.c"
                           PRIV_REQUIRES ${COMMON_COMPONENTS}
                           INCLUDE_DIRS ".")
elseif(APP_ROLE STREQUAL "receiver")
    idf_component_register(SRCS "receiver_main.c" "espnow_link.c"
                           PRIV_REQUIRES ${COMMON_COMPONENTS}
                           INCLUDE_DIRS ".")
else()
    message(FATAL_ERROR "APP_ROLE must be standalone, device, or receiver")
endif()
