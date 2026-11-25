idf_component_register(
    SRCS "main.c"
         "ble_bridge.c"
         "mic_i2s.c"
         "fft.c"
         "mfcc.c"
         "dtw.c"
         "asr.c"
    INCLUDE_DIRS "."
    REQUIRES bt
             nvs_flash
             esp_driver_i2s
             esp_driver_i2c
             esp_driver_gpio
             esp_timer
             esp_codec_dev
)

# 只给本工程源码开 -O2：MFCC 每 10ms 跑一次 512 点 FFT（100 次/秒）。
# 全局开 CONFIG_COMPILER_OPTIMIZATION_PERF 会让 mbedtls 的大文件在编译期
# 吃掉 1GB 以上内存并挂死（本机实测），所以优化只加在这一层。
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
