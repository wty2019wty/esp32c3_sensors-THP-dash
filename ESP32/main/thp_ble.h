/*
 * 多设备 BLE 网关编排：窗口扫描 → 按源逐台上报
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

bool thp_ble_is_ready(void);
void thp_ble_scan_window_open(int64_t cycle_ref_ms);
void thp_ble_scan_window_realign(int64_t cycle_ref_ms);
void thp_ble_scan_window_close(void);

/** 窗口关闭后调用：对每个 BLE 源取最佳帧并上报/入队 */
void thp_ble_report_cycle(int64_t cycle_ref_ms, TickType_t deadline);
