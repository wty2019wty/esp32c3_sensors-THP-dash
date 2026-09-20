/*
 * 离线补传队列（RAM 环形缓冲）
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
