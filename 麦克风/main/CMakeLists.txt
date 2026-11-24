idf_component_register(
    SRCS "main.c"
         "ble_bridge.c"
         "tts.c"
         "syn6288.c"
         "gb2312.c"
         "gb2312_table.c"
         "mic_i2s.c"
         "fft.c"
         "mfcc.c"
         "dtw.c"
         "asr.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             nvs_flash
             esp_driver_uart
             esp_driver_gpio
             esp_driver_i2s
             esp_timer
)

# 只给本工程自己的源文件开 -O2：MFCC 每 10ms 要跑一次 512 点 FFT（100 次/秒），
# 体积优先（-Os）下浮点运算会明显拖慢实时性。
# 全局开 -O2 会让 mbedtls 的大文件在编译期吃掉大量内存并挂死（本机实测），
# 所以优化只加在这一层组件上，不动 sdkconfig 的全局优化等级。
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
