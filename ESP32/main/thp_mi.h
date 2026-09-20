/*
 * 小米温湿度计（pvvx ATC BLE）上报路径
 */
#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thp_types.h"

/** Token + MAC 齐全且 BLE init 成功时返回 true */
bool thp_mi_init(void);
bool thp_mi_is_ready(void);

/**
 * 本周期：读 BLE 缓存 → 有样本则上报或入队。
 * deadline 耗尽时不发 HTTP，仍把有效样本推入离线队列。
 */
void thp_mi_report_cycle(TickType_t deadline);
