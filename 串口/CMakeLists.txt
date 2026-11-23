# ESP32-S31 BLE <-> UART0 桥接工程
# 目标芯片：esp32s31（ESP-IDF preview 目标，所有 idf.py 命令必须带 --preview）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(kskbl_s31_bridge)
