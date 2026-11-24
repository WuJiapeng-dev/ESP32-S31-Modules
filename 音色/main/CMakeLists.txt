idf_component_register(
    SRCS "main.c"
         "ble_bridge.c"
         "cfg_store.c"
         "wifi_prov.c"
         "audio_out.c"
         "ds_tts.c"
         "speak.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             nvs_flash
             esp_driver_i2s
             esp_driver_gpio
             esp_http_client
             esp_timer
             esp_wifi
             esp_netif
             esp_http_server
             mbedtls
)
