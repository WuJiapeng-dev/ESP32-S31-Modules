idf_component_register(
    SRCS "main.c"
         "mic_i2s.c"
         "audio_out.c"
         "asr_client.c"
         "llm_client.c"
         "ds_tts.c"
         "dialog.c"
         "cfg_store.c"
         "wifi_prov.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash
             esp_driver_i2s
             esp_driver_gpio
             esp_http_client
             esp_http_server
             esp_timer
             esp_wifi
             esp_netif
             mbedtls
             json
)
