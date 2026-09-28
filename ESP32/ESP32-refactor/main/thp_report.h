/*
 * HTTPS 上报：按 source_id 取 Token；重试；离线补传
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "thp_types.h"

esp_err_t thp_report_init(void);
void thp_report_new_cycle(void);

thp_http_result_t thp_report_with_retry(const thp_reading_t *r, bool backfill,
                                        int max_retries, TickType_t deadline);

void thp_report_or_enqueue(const thp_reading_t *r, TickType_t deadline);
void thp_report_flush_queue(int max_items, TickType_t deadline);
void thp_report_probe_api(void);
