/*
 * HTTPS 上报：JSON 组装、重试、离线补传
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "thp_types.h"

esp_err_t thp_report_init(void);

thp_http_result_t thp_report_with_retry(const thp_reading_t *r, bool backfill, int max_retries);

/** 实时上报；暂态失败入离线队列 */
void thp_report_or_enqueue(const thp_reading_t *r);

/** 网络恢复后按时间顺序补传，每周期最多 max_items 条 */
void thp_report_flush_queue(int max_items);

void thp_report_probe_api(void);
