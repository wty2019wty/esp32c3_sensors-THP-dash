/*
 * HTTPS 上报：JSON 组装、重试、离线补传
 *
 * deadline 为绝对 tick：到达后不再发起 HTTP；调用方仍应保证读数入队。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thp_types.h"

esp_err_t thp_report_init(void);

/** 每周期开始时调用：重置 probe 等周期内限流状态 */
void thp_report_new_cycle(void);

thp_http_result_t thp_report_with_retry(const thp_reading_t *r, bool backfill,
                                        int max_retries, TickType_t deadline);

/** 实时上报；Wi-Fi 不可用或 deadline 已到时直接入离线队列 */
void thp_report_or_enqueue(const thp_reading_t *r, TickType_t deadline);

/** 补传；达到 deadline 或 Wi-Fi 断开则停止，剩余下周期 */
void thp_report_flush_queue(int max_items, TickType_t deadline);

void thp_report_probe_api(void);
