/*
 * 离线补传队列（RAM 环形；条目带 source_id）
 *
 * peek_copy / pop 非原子：仅允许单一补传任务「peek → 网络 → 成功再 pop」。
 * AUTH 失效用 clear_source 原地压缩，不依赖 pop。
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
void thp_queue_clear_source(uint8_t source_id);
unsigned thp_queue_count(void);
