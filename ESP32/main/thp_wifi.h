/*
 * Wi-Fi STA：连接、占空比开关、状态查询
 *
 * 电池向：周期工作前 thp_wifi_radio_on，结束后 thp_wifi_radio_off，
 * 射频空闲关闭，配合 PM light sleep。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 首次启动 STA 并连上（或超时后台重连）；会设 PS 与 TX power */
esp_err_t thp_wifi_init_sta(void);

/** 周期工作前拉起射频并等待连接；已在连则只等待 */
esp_err_t thp_wifi_radio_on(uint32_t timeout_ms);

/** 周期结束后关射频，进入可 light sleep 的空闲 */
void thp_wifi_radio_off(void);

bool thp_wifi_is_connected(void);
bool thp_wifi_get_rssi(int *rssi);
