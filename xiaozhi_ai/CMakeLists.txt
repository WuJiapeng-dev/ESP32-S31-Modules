# 顶层工程：小智ai接入（ESP32-S31 语音对话：INMP441 采集 -> DeepSeek ASR/LLM/TTS -> MAX98357 播放）
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi_ai)
