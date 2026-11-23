idf_component_register(
    SRCS "main.c"
         "ble_bridge.c"
         "uart_link.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             nvs_flash
             esp_driver_uart
             esp_driver_gpio
             esp_timer
)
