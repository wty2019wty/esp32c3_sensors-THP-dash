/*
 * 上报周期调度：单任务流水线（Wi-Fi 占空比 + 空闲 light sleep）
 *
 * 每周期：
 *   T-8s  thp_wifi_radio_on
 *   T-5s  （若 MI）开 BLE 窗
 *   T     NTP → LOCAL 采样上报 →（若 MI）取帧上报 → 补传
 *   之后  thp_wifi_radio_off，vTaskDelay 空闲（PM light sleep）
 *
 * MI 关闭时跳过 BLE 窗与 T+10s 等待。
 * deadline = cycle_start + period - margin，绝对 tick 下沉到 report 层。
 */
#pragma once

void thp_sched_start_task(void);
