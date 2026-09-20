/*
 * 小米温湿度计（pvvx ATC BLE）上报路径
 */
#pragma once

#include <stdbool.h>

#include "thp_types.h"

/** Token + MAC 齐全且 BLE init 成功时返回 true */
bool thp_mi_init(void);
bool thp_mi_is_ready(void);

/** 本周期：扫描缓存取「本周期内最近一帧」→ 上报或入队 */
void thp_mi_report_cycle(void);
