/*
 * BMP280 气压传感器驱动（I2C 0x76 / 0x77）
 * 参考：Bosch BMP280 Datasheet Rev 1.1
 * 基础移植自：G:\esp32s3\esp32c3_sensors\components\bmp280
 *
 * THP 口径：pressure 仅来自 BMP280（REQUIREMENTS.md §4.2）。
 * BMP280 内部温度仅用于气压补偿 t_fine，不得写入业务 temperature 字段。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#define BMP280_I2C_ADDR             0x76
#define BMP280_I2C_ADDR_ALT         0x77

#define BMP280_REG_CHIP_ID          0xD0
#define BMP280_REG_RESET            0xE0
#define BMP280_RESET_MAGIC          0xB6
#define BMP280_REG_CALIB            0x88
#define BMP280_REG_STATUS           0xF3
#define BMP280_REG_CTRL_MEAS        0xF4
#define BMP280_REG_CONFIG           0xF5
#define BMP280_REG_PRESS_MSB        0xF7
#define BMP280_REG_DATA_LEN         6
#define BMP280_CHIP_ID              0x58
#define BMP280_CALIB_LEN            26
#define BMP280_STATUS_MEASURING     0x08
/* osrs_t=x2, osrs_p=x16, mode=forced — 按需测量，降低 continuous 自热 */
#define BMP280_CTRL_MEAS_SLEEP      0x00
#define BMP280_CTRL_MEAS_FORCED     0x55
/* IIR filter coefficient 4（气压） */
#define BMP280_CONFIG_FILTER4       0x0C
#define BMP280_FORCED_POLL_MS       5
#define BMP280_FORCED_TIMEOUT_MS    80
#define BMP280_SEA_LEVEL_PA         101325.0f
#define BMP280_ALT_EXPONENT         0.1903f
#define BMP280_PA_PER_HPA           100.0f
#define BMP280_I2C_TIMEOUT_MS       100

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    uint8_t  i2c_addr;          /* 实际生效地址 0x76 或 0x77 */
    i2c_master_dev_handle_t dev;
    bool present;
} bmp280_t;

esp_err_t bmp280_init(bmp280_t *bmp, i2c_master_bus_handle_t bus, uint32_t scl_speed_hz);
esp_err_t bmp280_read(bmp280_t *bmp, float *temp_c, float *press_hpa, float *alt_m, int32_t *t_fine);
void bmp280_compensate_temperature(const bmp280_t *bmp, int32_t adc_t, float *temp_c, int32_t *t_fine);
