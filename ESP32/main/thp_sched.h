/*
 * 上报周期调度：单任务流水线
 *
 * 每周期固定阶段：
 *   1. NTP 校时（Wi-Fi 可用时）
 *   2. 离线补传（预算条数）
 *   3. LOCAL I2C 采样 + 上报
 *   4. MI BLE 周期样本 + 上报（若就绪）
 *   5. 按周期睡眠（相对本周期起点，避免漂移）
 */
#pragma once

void thp_sched_start_task(void);
