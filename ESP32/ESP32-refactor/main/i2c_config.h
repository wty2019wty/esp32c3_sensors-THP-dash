/*
 * I2C 总线统一配置（SHT40 + BMP280 共用）
 *
 * 与参考工程 G:\esp32s3\esp32c3_sensors 保持一致：
 * ESP-IDF v6.x 新版 i2c_master 的时钟在设备配置 scl_speed_hz 中设置。
 * 引脚宏仅用于 main 中的总线初始化。
 */
#pragma once

#include "hal/gpio_types.h"

/* ESP32-C3 Super Mini 默认：SDA=GPIO8, SCL=GPIO9
 * 注意：GPIO8 可能接板载 LED；若 SDA 无法拉高，改到 GPIO6/GPIO7 并同步改接线。
 */
#define I2C_SDA_GPIO            GPIO_NUM_8
#define I2C_SCL_GPIO            GPIO_NUM_9

/* SHT40 / BMP280 均支持 Fast-mode 400kHz；排查时可降为 100000 */
#define I2C_SCL_SPEED_HZ        400000

/* 所有 I2C 操作统一超时（毫秒） */
#define I2C_BUS_TIMEOUT_MS      100
