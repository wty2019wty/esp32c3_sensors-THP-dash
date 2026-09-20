# THP Dash 设备端：ESP32-C3 温湿度气压 HTTPS 上报
# 参考工程：G:\esp32s3\esp32c3_sensors（ESP-IDF v6.1 / 新版 i2c_master）
# 构建系统：ESP-IDF CMake
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(esp32c3_thp_report)
