#include "thp_mi.h"

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "thp_config.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_time.h"
#include "thp_types.h"

static const char *TAG = "thp.mi";

#ifndef THP_MI_ENABLE
#define THP_MI_ENABLE 0
#endif
#ifndef THP_MI_DEVICE_TOKEN
#define THP_MI_DEVICE_TOKEN "thp_replace_me_mi"
#endif
#ifndef THP_MI_DEVICE_ID
#define THP_MI_DEVICE_ID ""
#endif
#ifndef THP_MI_MAC
#define THP_MI_MAC ""
#endif
#ifndef THP_MI_BINDKEY
#define THP_MI_BINDKEY ""
#endif
#ifndef THP_MI_MAX_AGE_MS
#define THP_MI_MAX_AGE_MS (3 * 5 * 60 * 1000)
#endif
#ifndef THP_MI_HEAP_MIN_REPORT
#define THP_MI_HEAP_MIN_REPORT 70000
#endif

#if THP_MI_ENABLE
#include "atc_ble.h"

static bool s_mi_ready;
/** 上次 MI 上报尝试之后的起点；只采用 ts >= 该值 的样本 */
static int64_t s_mi_period_start_ms;

static bool mi_token_ok(void)
{
    return THP_MI_DEVICE_TOKEN[0] != '\0' &&
           strcmp(THP_MI_DEVICE_TOKEN, "thp_replace_me_mi") != 0 &&
           strlen(THP_MI_DEVICE_TOKEN) >= 8;
}

static bool mi_pick_period_sample(atc_ble_sample_t *out)
{
    if (!s_mi_ready) {
        return false;
    }
    atc_ble_sample_t s;
    if (!atc_ble_pop_latest(&s) || !s.valid) {
        return false;
    }
    if (!thp_th_in_range(s.temperature, s.humidity)) {
        return false;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if ((now_ms - s.ts_ms) > THP_MI_MAX_AGE_MS) {
        ESP_LOGW(TAG, "MI 缓存过期 age=%lldms，本周期跳过",
                 (long long)(now_ms - s.ts_ms));
        return false;
    }
    if (s.ts_ms < s_mi_period_start_ms) {
        ESP_LOGW(TAG, "MI 本周期无新帧（最近 age=%lldms < 周期起点），跳过",
                 (long long)(now_ms - s.ts_ms));
        return false;
    }
    *out = s;
    return true;
}

bool thp_mi_init(void)
{
    if (!mi_token_ok()) {
        ESP_LOGW(TAG, "MI Token 未配置（THP_MI_DEVICE_TOKEN），跳过小米计上报");
        return false;
    }

    uint8_t mac[6];
    uint8_t key[16];
    bool mac_ok = atc_ble_parse_mac_str(THP_MI_MAC, mac);
    if (!mac_ok) {
        ESP_LOGW(TAG, "THP_MI_MAC 非法: '%s'（示例 A4:C1:38:E2:4E:43）", THP_MI_MAC);
        return false;
    }
    bool key_ok = atc_ble_parse_key_hex(THP_MI_BINDKEY, key);
    if (!key_ok) {
        memset(key, 0, sizeof(key));
        ESP_LOGW(TAG, "THP_MI_BINDKEY 非法或未填；明文 Custom 可工作，加密 beacon 需要 BindKey");
    }

    esp_err_t mi_err = atc_ble_init(mac, key_ok ? key : NULL);
    if (mi_err != ESP_OK) {
        ESP_LOGE(TAG, "atc_ble_init 失败: %s", esp_err_to_name(mi_err));
        return false;
    }

    s_mi_ready = true;
    s_mi_period_start_ms = 0; /* 首周期接受任意已缓存/新扫到的帧 */
    ESP_LOGI(TAG, "MI BLE 网关就绪（持续扫描） mac=%s enc_key=%d", THP_MI_MAC, (int)key_ok);
    return true;
}

bool thp_mi_is_ready(void)
{
    return s_mi_ready;
}

void thp_mi_report_cycle(TickType_t deadline)
{
    if (!s_mi_ready) {
        return;
    }

    if (!atc_ble_is_scanning()) {
        (void)atc_ble_start_scan();
    }

    atc_ble_sample_t mi;
    if (!mi_pick_period_sample(&mi)) {
        s_mi_period_start_ms = esp_timer_get_time() / 1000;
        return;
    }

    thp_reading_t reading;
    thp_time_stamp_reading(&reading);
    reading.kind = THP_KIND_MI;
    reading.temperature = mi.temperature;
    reading.humidity = mi.humidity;
    reading.pressure = 0;
    reading.has_th = true;
    reading.has_p = false;
    reading.rssi = (int)mi.rssi;

    unsigned heap = (unsigned)esp_get_free_heap_size();
    const int32_t remain = thp_deadline_remain_ms(deadline);

    ESP_LOGI(TAG, "MI 本周期样本 T=%.2f°C H=%.2f%% rssi=%d batt=%u age=%lldms iso=%s heap=%u remain=%dms",
             (double)mi.temperature, (double)mi.humidity,
             (int)mi.rssi,
             mi.battery_pct == 0xFF ? 0 : mi.battery_pct,
             (long long)((esp_timer_get_time() / 1000) - mi.ts_ms),
             reading.has_iso ? reading.iso : "(no-ts)",
             heap, (int)remain);

    /* heap 不足或预算耗尽：不发 HTTP，但样本仍入队，避免丢数据 */
    if (heap < THP_MI_HEAP_MIN_REPORT || thp_deadline_reached(deadline) ||
        !thp_wifi_is_connected()) {
        if (heap < THP_MI_HEAP_MIN_REPORT) {
            ESP_LOGW(TAG, "heap_free=%u < %d，MI 读数直接入队", heap, THP_MI_HEAP_MIN_REPORT);
        }
        thp_queue_push(&reading);
        s_mi_period_start_ms = esp_timer_get_time() / 1000;
        return;
    }

    thp_report_or_enqueue(&reading, deadline);
    s_mi_period_start_ms = esp_timer_get_time() / 1000;
}

#else /* !THP_MI_ENABLE */

bool thp_mi_init(void)
{
    ESP_LOGI(TAG, "THP_MI_ENABLE=0，未编译小米计 BLE 网关");
    return false;
}

bool thp_mi_is_ready(void)
{
    return false;
}

void thp_mi_report_cycle(TickType_t deadline)
{
    (void)deadline;
}

#endif /* THP_MI_ENABLE */
