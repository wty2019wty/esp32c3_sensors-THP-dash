/*
 * 上报周期调度：单任务流水线
 *
 * 每周期固定阶段（LOCAL/MI 优先，补传后置）：
 *   1. NTP（已同步则短路；每 N 周期重同步）
 *   2. LOCAL I2C 采样 → 上报或入队（deadline 耗尽仍入队）
 *   3. MI BLE 缓存 → 上报或入队（同上）
 *   4. 离线补传（受 deadline 预算约束）
 *   5. 按周期睡眠（相对本周期起点，避免漂移）
 *
 * deadline = cycle_start + period - margin，绝对 tick 下沉到 report 层。
 */
#pragma once

void thp_sched_start_task(void);
