/*
 * BMP280 气压传感器驱动实现（ESP-IDF 新版 I2C master API）
 * 基础移植自：G:\esp32s3\esp32c3_sensors\components\bmp280
 *
 * 变更：按 REQUIREMENTS.md §4.1 依次探测 0x76 / 0x77。
 * 气压补偿依赖温度 t_fine；对外 temperature 字段不使用 BMP280 温度。
 */
#include "bmp280.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bmp280";

static esp_err_t bmp280_read_regs(bmp280_t *bmp, uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(bmp->dev, &reg, 1, buf, len, BMP280_I2C_TIMEOUT_MS);
}

static esp_err_t bmp280_write_reg(bmp280_t *bmp, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(bmp->dev, buf, sizeof(buf), BMP280_I2C_TIMEOUT_MS);
}

static void bmp280_parse_calib(bmp280_t *bmp, const uint8_t *d)
{
    bmp->dig_T1 = (uint16_t)(((uint16_t)d[1] << 8) | d[0]);
    bmp->dig_T2 = (int16_t)(((uint16_t)d[3] << 8) | d[2]);
    bmp->dig_T3 = (int16_t)(((uint16_t)d[5] << 8) | d[4]);

    bmp->dig_P1 = (uint16_t)(((uint16_t)d[7] << 8) | d[6]);
    bmp->dig_P2 = (int16_t)(((uint16_t)d[9] << 8) | d[8]);
    bmp->dig_P3 = (int16_t)(((uint16_t)d[11] << 8) | d[10]);
    bmp->dig_P4 = (int16_t)(((uint16_t)d[13] << 8) | d[12]);
    bmp->dig_P5 = (int16_t)(((uint16_t)d[15] << 8) | d[14]);
    bmp->dig_P6 = (int16_t)(((uint16_t)d[17] << 8) | d[16]);
    bmp->dig_P7 = (int16_t)(((uint16_t)d[19] << 8) | d[18]);
    bmp->dig_P8 = (int16_t)(((uint16_t)d[21] << 8) | d[20]);
    bmp->dig_P9 = (int16_t)(((uint16_t)d[23] << 8) | d[22]);
}

void bmp280_compensate_temperature(const bmp280_t *bmp, int32_t adc_t, float *temp_c, int32_t *t_fine)
{
    double var1 = ((double)adc_t / 16384.0 - (double)bmp->dig_T1 / 1024.0) * (double)bmp->dig_T2;
    double var2 = ((double)adc_t / 131072.0 - (double)bmp->dig_T1 / 8192.0) *
                  ((double)adc_t / 131072.0 - (double)bmp->dig_T1 / 8192.0) * (double)bmp->dig_T3;

    *t_fine = (int32_t)(var1 + var2);
    *temp_c = (float)((var1 + var2) / 5120.0);
}

/**
 * @brief 探测并初始化 BMP280（0x76 → 0x77）
 */
esp_err_t bmp280_init(bmp280_t *bmp, i2c_master_bus_handle_t bus, uint32_t scl_speed_hz)
{
    if (bmp == NULL || bus == NULL || scl_speed_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(bmp, 0, sizeof(*bmp));
    bmp->present = false;

    static const uint8_t addrs[] = { BMP280_I2C_ADDR, BMP280_I2C_ADDR_ALT };
    uint8_t chosen = 0;
    bool found = false;

    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        if (i2c_master_probe(bus, addrs[i], BMP280_I2C_TIMEOUT_MS) == ESP_OK) {
            chosen = addrs[i];
            found = true;
            break;
        }
    }
    if (!found) {
        ESP_LOGE(TAG, "BMP280 未找到 (尝试 0x%02X/0x%02X)", BMP280_I2C_ADDR, BMP280_I2C_ADDR_ALT);
        return ESP_ERR_NOT_FOUND;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = chosen,
        .scl_speed_hz = scl_speed_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &bmp->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMP280 添加设备失败: %s", esp_err_to_name(err));
        return err;
    }
    bmp->i2c_addr = chosen;

    uint8_t chip_id = 0;
    err = bmp280_read_regs(bmp, BMP280_REG_CHIP_ID, &chip_id, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取 WHOAMI 失败: %s", esp_err_to_name(err));
        return err;
    }
    if (chip_id != BMP280_CHIP_ID) {
        ESP_LOGE(TAG, "WHOAMI 不匹配: 0x%02X (期望 0x%02X) @0x%02X",
                 chip_id, BMP280_CHIP_ID, chosen);
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint8_t calib[BMP280_CALIB_LEN] = {0};
    err = bmp280_read_regs(bmp, BMP280_REG_CALIB, calib, sizeof(calib));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取校准参数失败: %s", esp_err_to_name(err));
        return err;
    }
    bmp280_parse_calib(bmp, calib);

    /* CONFIG 仅在 sleep 下可写；先 sleep，再滤波，读时 forced */
    err = bmp280_write_reg(bmp, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_SLEEP);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 CTRL_MEAS(sleep) 失败: %s", esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));
    err = bmp280_write_reg(bmp, BMP280_REG_CONFIG, BMP280_CONFIG_FILTER4);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 CONFIG 失败: %s", esp_err_to_name(err));
        return err;
    }

    bmp->present = true;
    ESP_LOGI(TAG, "BMP280 初始化成功 (0x%02X), WHOAMI=0x%02X, forced+IIR4", chosen, chip_id);
    return ESP_OK;
}

esp_err_t bmp280_read(bmp280_t *bmp, float *temp_c, float *press_hpa, float *alt_m, int32_t *t_fine)
{
    if (bmp == NULL || temp_c == NULL || press_hpa == NULL || alt_m == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!bmp->present) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = bmp280_write_reg(bmp, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_FORCED);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "触发 forced 测量失败: %s", esp_err_to_name(err));
        return err;
    }

    /* 等 measuring 位置位后再清零，避免刚触发就读到旧数据 */
    vTaskDelay(pdMS_TO_TICKS(10));
    int waited = 10;
    while (waited <= BMP280_FORCED_TIMEOUT_MS) {
        uint8_t st = 0;
        err = bmp280_read_regs(bmp, BMP280_REG_STATUS, &st, 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "读取 STATUS 失败: %s", esp_err_to_name(err));
            return err;
        }
        if ((st & BMP280_STATUS_MEASURING) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(BMP280_FORCED_POLL_MS));
        waited += BMP280_FORCED_POLL_MS;
    }
    if (waited > BMP280_FORCED_TIMEOUT_MS) {
        ESP_LOGE(TAG, "forced 测量超时");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t d[BMP280_REG_DATA_LEN] = {0};
    err = bmp280_read_regs(bmp, BMP280_REG_PRESS_MSB, d, sizeof(d));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取测量数据失败: %s", esp_err_to_name(err));
        return err;
    }

    int32_t adc_p = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | ((uint32_t)d[2] >> 4));
    int32_t adc_t = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | ((uint32_t)d[5] >> 4));

    int32_t fine = 0;
    float temperature = 0.0f;
    bmp280_compensate_temperature(bmp, adc_t, &temperature, &fine);

    if (t_fine != NULL) {
        *t_fine = fine;
    }
    *temp_c = temperature;

    double var1 = ((double)fine / 2.0) - 64000.0;
    double var2 = var1 * var1 * (double)bmp->dig_P6 / 32768.0;
    var2 = var2 + var1 * (double)bmp->dig_P5 * 2.0;
    var2 = (var2 / 4.0) + ((double)bmp->dig_P4 * 65536.0);
    var1 = ((double)bmp->dig_P3 * var1 * var1 / 524288.0 + (double)bmp->dig_P2 * var1) / 524288.0;
    var1 = (1.0 + var1 / 32768.0) * (double)bmp->dig_P1;

    if (var1 == 0.0) {
        ESP_LOGE(TAG, "气压补偿失败：除数为 0（校准参数异常）");
        return ESP_ERR_INVALID_STATE;
    }

    double p = 1048576.0 - (double)adc_p;
    p = (p - (var2 / 4096.0)) * 6250.0 / var1;
    var1 = (double)bmp->dig_P9 * p * p / 2147483648.0;
    var2 = p * (double)bmp->dig_P8 / 32768.0;
    p = p + (var1 + var2 + (double)bmp->dig_P7) / 16.0;

    float press_pa = (float)p;
    *press_hpa = press_pa / BMP280_PA_PER_HPA;

    *alt_m = 44330.0f * (1.0f - powf(press_pa / BMP280_SEA_LEVEL_PA, BMP280_ALT_EXPONENT));

    return ESP_OK;
}
