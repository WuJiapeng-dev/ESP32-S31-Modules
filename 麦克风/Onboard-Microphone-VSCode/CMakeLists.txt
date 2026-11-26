# 顶层工程：board_mic（ESP32-S31 麦克风识别 → BLE 送手机显示）
#
# 与 ../Speaker-Repeat-VSCode 的区别：本工程**不接喇叭**，没有 UART/TTS/BY，
# 识别结果只经 BLE Notify 推给手机。
#
# 工程名保持 ASCII：ESP-IDF 工具链在非 ASCII 路径下会失败
# （gcc 的 -specs 参数里中文被转成换行符，报 cannot read spec file）。
# 目录名用英文直译功能名（麦克风/Onboard-Microphone-VSCode/），工程名仍保持 ASCII。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(board_mic)
