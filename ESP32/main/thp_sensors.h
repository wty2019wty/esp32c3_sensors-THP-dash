/*
 * I2C + SHT40/BMP280 采样（THP 口径）
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#include "thp_types.h"

esp_err_t thp_sensors_init(void);
bool thp_sensors_present_th(void);
bool thp_sensors_present_p(void);

/**
 * @brief 采样；允许仅温湿度或仅气压
 * @return true：至少一组字段有效
 */
bool thp_sensors_sample(thp_sample_t *out);
