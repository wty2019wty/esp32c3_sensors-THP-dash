/*
 * 上报周期调度：单任务流水线
 *
 * BLE 窗口：周期起点 T 前 5s 开扫，T+10s 关窗；MI 取距 T 最近帧。
 * 每周期阶段：
 *   0. T-5s 开 BLE 窗（默认不扫，仅窗口内扫描）
 *   1. T 时刻：NTP（已同步则短路）
 *   2. LOCAL I2C 采样 → 上报或入队
 *   3. 等到 T+10s 关窗 → MI 取距 T 最近帧 → 上报或入队
 *   4. 离线补传（deadline 预算内）
 *   5. 睡到下一周期 T'-5s
 *
 * deadline = cycle_start + period - margin，绝对 tick 下沉到 report 层。
 */
#pragma once

void thp_sched_start_task(void);
