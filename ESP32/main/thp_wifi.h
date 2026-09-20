/*
 * Wi-Fi STA：连接、状态查询、RSSI
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t thp_wifi_init_sta(void);
bool thp_wifi_is_connected(void);
bool thp_wifi_get_rssi(int *rssi);
