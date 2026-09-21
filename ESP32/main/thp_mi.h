/*
 * 小米温湿度计（pvvx ATC BLE）上报路径
 *
 * 扫描策略：仅在周期起点 T 的 [T-5s, T+10s] 窗口内扫描；
 * 上报取窗口内距 T 最近的一帧，不使用持续扫描。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thp_types.h"

/** Token + MAC 齐全且 BLE init 成功时返回 true */
bool thp_mi_init(void);
bool thp_mi_is_ready(void);

/** 在 T-5s 调用：开启扫描窗口并停在扫描态（直到 window_close） */
void thp_mi_scan_window_open(int64_t cycle_ref_ms);

/** 周期实际起点确定后调用：窗边界对齐到与取帧相同的 cycle_ref_ms */
void thp_mi_scan_window_realign(int64_t cycle_ref_ms);

/** 周期采集结束后调用：关窗并停扫（窗口外不扫描） */
void thp_mi_scan_window_close(void);

/**
 * 本周期上报：取窗口内距 cycle_ref_ms 最近的一帧 → 上报或入队。
 * 须在窗口关闭后调用。deadline 耗尽时不发 HTTP，有效样本仍入队。
 */
void thp_mi_report_cycle(int64_t cycle_ref_ms, TickType_t deadline);
