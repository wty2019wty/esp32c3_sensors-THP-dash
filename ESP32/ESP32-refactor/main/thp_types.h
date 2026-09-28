/*
 * THP 多设备共享类型
 */
#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ISO_UTC_BUF_LEN 32

/** 绝对 tick deadline：已到/已过返回 true（tick 回绕安全） */
static inline bool thp_deadline_reached(TickType_t deadline)
{
    return (int32_t)(xTaskGetTickCount() - deadline) >= 0;
}

/** 距 deadline 剩余毫秒；已到则为 0 */
static inline int32_t thp_deadline_remain_ms(TickType_t deadline)
{
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - deadline) >= 0) {
        return 0;
    }
    return (int32_t)((deadline - now) * portTICK_PERIOD_MS);
}

typedef enum {
    THP_SRC_LOCAL = 0,
    THP_SRC_BLE   = 1,
} thp_src_kind_t;

/** 一帧业务读数；source_id 见 thp_sources */
typedef struct {
    uint8_t source_id;
    float   temperature;
    float   humidity;
    float   pressure;
    bool    has_th;
    bool    has_p;
    int     rssi;
    char    iso[ISO_UTC_BUF_LEN];
    bool    has_iso;
} thp_reading_t;

/** 本机 I2C 采样结果 */
typedef struct {
    float temperature;
    float humidity;
    float pressure;
    bool  has_th;
    bool  has_p;
    bool  valid;
} thp_sample_t;

typedef enum {
    THP_HTTP_OK = 0,
    THP_HTTP_AUTH_FAIL,
    THP_HTTP_BAD_PAYLOAD,
    THP_HTTP_TRANSIENT,
} thp_http_result_t;

/** 与 Worker validateReadingPayload 对齐 */
static inline bool thp_th_in_range(float t, float h)
{
    return t >= -40.0f && t <= 85.0f && h >= 0.0f && h <= 100.0f;
}

static inline bool thp_p_in_range(float p)
{
    return p >= 300.0f && p <= 1200.0f;
}
