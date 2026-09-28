/*
 * 周期调度：单任务流水线（多源）
 *
 * BLE 窗：T-5s 开扫，T+10s 关窗；各 BLE 源取距 T 最近帧。
 * 阶段：校时 → LOCAL → 等窗+BLE 逐台 → 补传 → 睡到下窗
 */
#pragma once

void thp_sched_start_task(void);
