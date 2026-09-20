/*
 * SHT40 温湿度传感器驱动（I2C 地址 0x44）
 * 参考：Sensirion SHT4x Datasheet
 * 移植自：G:\esp32s3\esp32c3_sensors\components\sht40
 *
 * THP 口径：temperature / humidity 仅来自 SHT40（REQUIREMENTS.md §4.2）
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define SHT40_I2C_ADDR              0x44
#define SHT40_I2C_ADDR_ALT          0x45
#define SHT40_CMD_MEASURE_HIGH_PREC 0xFD
#define SHT40_MEASURE_DELAY_MS      10
#define SHT40_CRC_POLY              0x31
#define SHT40_CRC_INIT              0xFF
#define SHT40_RAW_BYTES             6
#define SHT40_I2C_TIMEOUT_MS        100

typedef struct {
    i2c_master_dev_handle_t dev;
    bool present;
} sht40_t;

esp_err_t sht40_init(sht40_t *sht, i2c_master_bus_handle_t bus, uint32_t scl_speed_hz);
esp_err_t sht40_read(sht40_t *sht, float *temp_c, float *humi_rh);
uint8_t sht40_crc8(const uint8_t *data, size_t len);
