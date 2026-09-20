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
#include "thp_wifi.h"

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
#ifndef THP_MI_SCAN_OPEN_BEFORE_MS
#define THP_MI_SCAN_OPEN_BEFORE_MS 5000
#endif
#ifndef THP_MI_SCAN_CLOSE_AFTER_MS
#define THP_MI_SCAN_CLOSE_AFTER_MS 10000
#endif

#if THP_MI_ENABLE
#include "atc_ble.h"

static bool s_mi_ready;

static bool mi_token_ok(void)
{
    return THP_MI_DEVICE_TOKEN[0] != '\0' &&
           strcmp(THP_MI_DEVICE_TOKEN, "thp_replace_me_mi") != 0 &&
           strlen(THP_MI_DEVICE_TOKEN) >= 8;
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
    ESP_LOGI(TAG, "MI BLE 网关就绪（窗口扫描 T-%dms~T+%dms） mac=%s enc_key=%d",
             THP_MI_SCAN_OPEN_BEFORE_MS, THP_MI_SCAN_CLOSE_AFTER_MS,
             THP_MI_MAC, (int)key_ok);
    return true;
}

bool thp_mi_is_ready(void)
{
    return s_mi_ready;
}

void thp_mi_scan_window_open(int64_t cycle_ref_ms)
{
    if (!s_mi_ready) {
        return;
    }
    atc_ble_window_open(cycle_ref_ms,
                        THP_MI_SCAN_OPEN_BEFORE_MS,
                        THP_MI_SCAN_CLOSE_AFTER_MS);
}

void thp_mi_scan_window_close(void)
{
    if (!s_mi_ready) {
        return;
    }
    atc_ble_window_close();
}

void thp_mi_report_cycle(int64_t cycle_ref_ms, TickType_t deadline)
{
    if (!s_mi_ready) {
        return;
    }

    atc_ble_sample_t mi;
    if (!atc_ble_pop_window_best(cycle_ref_ms, &mi) || !mi.valid) {
        ESP_LOGW(TAG, "MI 窗口内无样本（T-%dms~T+%dms），本周期跳过",
                 THP_MI_SCAN_OPEN_BEFORE_MS, THP_MI_SCAN_CLOSE_AFTER_MS);
        return;
    }
    if (!thp_th_in_range(mi.temperature, mi.humidity)) {
        ESP_LOGW(TAG, "MI 窗口样本超范围，丢弃 T=%.2f H=%.2f",
                 (double)mi.temperature, (double)mi.humidity);
        return;
    }

    const int64_t dist_ms = mi.ts_ms - cycle_ref_ms;
    const int64_t abs_dist = dist_ms < 0 ? -dist_ms : dist_ms;
    if (abs_dist > THP_MI_MAX_AGE_MS) {
        ESP_LOGW(TAG, "MI 样本距周期起点过远 dist=%lldms，跳过",
                 (long long)abs_dist);
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

    ESP_LOGI(TAG, "MI 窗口样本 T=%.2f°C H=%.2f%% rssi=%d batt=%u dist_to_T=%+lldms iso=%s heap=%u remain=%dms",
             (double)mi.temperature, (double)mi.humidity,
             (int)mi.rssi,
             mi.battery_pct == 0xFF ? 0 : mi.battery_pct,
             (long long)dist_ms,
             reading.has_iso ? reading.iso : "(no-ts)",
             heap, (int)thp_deadline_remain_ms(deadline));

    if (heap < THP_MI_HEAP_MIN_REPORT || thp_deadline_reached(deadline) ||
        !thp_wifi_is_connected()) {
        if (heap < THP_MI_HEAP_MIN_REPORT) {
            ESP_LOGW(TAG, "heap_free=%u < %d，MI 读数直接入队", heap, THP_MI_HEAP_MIN_REPORT);
        }
        thp_queue_push(&reading);
        return;
    }

    thp_report_or_enqueue(&reading, deadline);
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

void thp_mi_scan_window_open(int64_t cycle_ref_ms)
{
    (void)cycle_ref_ms;
}

void thp_mi_scan_window_close(void)
{
}

void thp_mi_report_cycle(int64_t cycle_ref_ms, TickType_t deadline)
{
    (void)cycle_ref_ms;
    (void)deadline;
}

#endif /* THP_MI_ENABLE */
