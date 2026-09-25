/*
 * 离线补传队列（RTC 慢速内存环形缓冲）
 *
 * deep sleep 后保留；上电复位（非 timer 唤醒）时 magic 失效则清空。
 * 容量 THP_OFFLINE_QUEUE_LEN（默认 16，RTC 约 8KB 内）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "thp_types.h"

esp_err_t thp_queue_init(void);
void thp_queue_push(const thp_reading_t *r);
bool thp_queue_peek_copy(thp_reading_t *out);
void thp_queue_pop(void);
void thp_queue_clear(void);
unsigned thp_queue_count(void);
