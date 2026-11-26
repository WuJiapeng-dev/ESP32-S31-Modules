# 顶层工程：maikefeng（ESP32-S31 麦克风语音复述）
#
# 工程名保持 ASCII：ESP-IDF 工具链在非 ASCII 路径下会失败
# （gcc 的 -specs 参数里中文被转成换行符，报 cannot read spec file）。
# 目录名用英文直译功能名（麦克风/Speaker-Repeat-VSCode/），工程名仍保持 ASCII。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(maikefeng)
