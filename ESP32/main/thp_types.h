/*
 * THP 设备端共享类型与小工具
 */
#pragma once

#include <stdbool.h>
#include <stdio.h>

#define ISO_UTC_BUF_LEN 32

typedef enum {
    THP_KIND_LOCAL = 0,
    THP_KIND_MI    = 1,
} thp_device_kind_t;

typedef struct {
    float temperature;  /* SHT40 */
    float humidity;     /* SHT40 */
    float pressure;     /* BMP280 hPa */
    bool  has_th;
    bool  has_p;
    bool  valid;        /* 至少一组字段有效 */
} thp_sample_t;

/** 一帧待上报/待补传读数；iso 为采样时刻 UTC（补传时写入 ts） */
typedef struct {
    thp_device_kind_t kind;
    float temperature;
    float humidity;
    float pressure;
    bool has_th;
    bool has_p;
    int rssi;
    char iso[ISO_UTC_BUF_LEN];
    bool has_iso;
} thp_reading_t;

typedef enum {
    THP_HTTP_OK = 0,
    THP_HTTP_AUTH_FAIL,     /* 401/403：Token 无效/吊销，不重试 */
    THP_HTTP_BAD_PAYLOAD,   /* 400：载荷不合法，不重试 */
    THP_HTTP_TRANSIENT,     /* 网络/5xx：有限重试 */
} thp_http_result_t;

static inline const char *thp_kind_tag(thp_device_kind_t k)
{
    return (k == THP_KIND_MI) ? "MI" : "LOCAL";
}

/** 与 Worker validateReadingPayload 对齐（src/routes/readings.js） */
static inline bool thp_th_in_range(float t, float h)
{
    return t >= -40.0f && t <= 85.0f && h >= 0.0f && h <= 100.0f;
}

static inline bool thp_p_in_range(float p)
{
    return p >= 300.0f && p <= 1200.0f;
}
