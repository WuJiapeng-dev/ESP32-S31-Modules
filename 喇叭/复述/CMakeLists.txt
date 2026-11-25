# 顶层工程：maikefeng（ESP32-S31 麦克风语音复述）
#
# 工程名保持 ASCII：ESP-IDF 工具链在非 ASCII 路径下会失败
# （gcc 的 -specs 参数里中文被转成换行符，报 cannot read spec file）。
# 中文名只出现在 GitHub 的目录名（麦克风/）上。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(maikefeng)
