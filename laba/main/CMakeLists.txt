idf_component_register(
    SRCS "main.c"
         "ble_bridge.c"
         "tts.c"
         "syn6288.c"
         "gb2312.c"
         "gb2312_table.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             nvs_flash
             esp_driver_uart
             esp_driver_gpio
             esp_timer
)
