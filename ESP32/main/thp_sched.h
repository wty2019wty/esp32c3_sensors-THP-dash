/*
 * 上报工作：单次唤醒周期（deep sleep 之间）
 *
 * 唤醒 → Wi-Fi/NTP → LOCAL 采样上报 →（MI 若开）→ 补传 → 返回；
 * 由 main 计算剩余睡眠并 esp_deep_sleep。
 * MI 关闭时无 BLE 窗。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/** 本唤醒是否来自 deep sleep timer 唤醒（而非上电/复位） */
bool thp_sched_wake_from_sleep(void);

/**
 * 跑完一个完整上报周期（采样、上报/入队、补传）。
 * 调用前 Wi-Fi 应已连接；结束时不关射频（由 main 统一 radio_off）。
 */
void thp_sched_run_once(void);
