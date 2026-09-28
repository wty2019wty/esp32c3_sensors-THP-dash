/*
 * 本机 I2C 源：SHT40 温湿度 + BMP280 气压
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "thp_types.h"

esp_err_t thp_local_init(void);
bool thp_local_present_th(void);
bool thp_local_present_p(void);
bool thp_local_sample(thp_sample_t *out);
