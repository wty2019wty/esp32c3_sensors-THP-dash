#include "thp_ble.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "atc_ble.h"
#include "thp_config.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_sources.h"
#include "thp_time.h"
#include "thp_types.h"
#include "thp_wifi.h"

static const char *TAG = "thp.ble";

#ifndef THP_BLE_ENABLE
#define THP_BLE_ENABLE 0
#endif
#ifndef THP_BLE_SCAN_OPEN_BEFORE_MS
#define THP_BLE_SCAN_OPEN_BEFORE_MS 5000
#endif
#ifndef THP_BLE_SCAN_CLOSE_AFTER_MS
#define THP_BLE_SCAN_CLOSE_AFTER_MS 10000
#endif
#ifndef THP_BLE_MAX_AGE_MS
#define THP_BLE_MAX_AGE_MS (3 * 5 * 60 * 1000)
#endif
#ifndef THP_BLE_HEAP_MIN_REPORT
#define THP_BLE_HEAP_MIN_REPORT 70000
#endif

bool thp_ble_is_ready(void)
{
#if THP_BLE_ENABLE
    return atc_ble_device_count() > 0;
#else
    return false;
#endif
}

void thp_ble_scan_window_open(int64_t cycle_ref_ms)
{
    if (!thp_ble_is_ready()) {
        return;
    }
    atc_ble_window_open(cycle_ref_ms,
                        THP_BLE_SCAN_OPEN_BEFORE_MS,
                        THP_BLE_SCAN_CLOSE_AFTER_MS);
}

void thp_ble_scan_window_realign(int64_t cycle_ref_ms)
{
    if (!thp_ble_is_ready()) {
        return;
    }
    atc_ble_window_realign(cycle_ref_ms);
}

void thp_ble_scan_window_close(void)
{
    if (!thp_ble_is_ready()) {
        return;
    }
    atc_ble_window_close();
}

static void report_one_ble(uint8_t source_id, int64_t cycle_ref_ms, TickType_t deadline)
{
    int di = thp_source_ble_index(source_id);
    if (di < 0) {
        return;
    }

    atc_ble_sample_t mi;
    if (!atc_ble_pop_window_best((size_t)di, cycle_ref_ms, &mi) || !mi.valid) {
        ESP_LOGW(TAG, "BLE[%s] 窗口内无样本，本周期跳过", thp_source_name(source_id));
        return;
    }
    if (!thp_th_in_range(mi.temperature, mi.humidity)) {
        ESP_LOGW(TAG, "BLE[%s] 样本超范围 T=%.2f H=%.2f",
                 thp_source_name(source_id),
                 (double)mi.temperature, (double)mi.humidity);
        return;
    }

    const int64_t dist_ms = mi.ts_ms - cycle_ref_ms;
    const int64_t abs_dist = dist_ms < 0 ? -dist_ms : dist_ms;
    if (abs_dist > THP_BLE_MAX_AGE_MS) {
        ESP_LOGW(TAG, "BLE[%s] 样本距 T 过远 dist=%lldms，跳过",
                 thp_source_name(source_id), (long long)abs_dist);
        return;
    }

    thp_reading_t reading;
    thp_time_stamp_reading(&reading);
    reading.source_id = source_id;
    reading.temperature = mi.temperature;
    reading.humidity = mi.humidity;
    reading.pressure = 0;
    reading.has_th = true;
    reading.has_p = false;
    reading.rssi = (int)mi.rssi;
    /* iso 对齐帧接收时刻（mi.ts_ms），而不是组帧/HTTP 时刻 */
    thp_time_fill_sample_iso(&reading, mi.ts_ms);

    unsigned heap = (unsigned)esp_get_free_heap_size();
    ESP_LOGI(TAG, "BLE[%s] 窗口样本 T=%.2f°C H=%.2f%% rssi=%d dist_to_T=%+lldms heap=%u remain=%dms",
             thp_source_name(source_id),
             (double)mi.temperature, (double)mi.humidity,
             (int)mi.rssi, (long long)dist_ms, heap,
             (int)thp_deadline_remain_ms(deadline));

    if (heap < THP_BLE_HEAP_MIN_REPORT || thp_deadline_reached(deadline) ||
        !thp_wifi_is_connected()) {
        thp_queue_push(&reading);
        return;
    }
    thp_report_or_enqueue(&reading, deadline);
}

void thp_ble_report_cycle(int64_t cycle_ref_ms, TickType_t deadline)
{
    if (!thp_ble_is_ready()) {
        return;
    }
    const size_t n = thp_source_count();
    for (size_t i = 0; i < n; i++) {
        if (thp_source_kind((uint8_t)i) != THP_SRC_BLE) {
            continue;
        }
        if (!thp_source_ready((uint8_t)i)) {
            continue;
        }
        if (thp_deadline_reached(deadline)) {
            ESP_LOGW(TAG, "deadline 已到，剩余 BLE 源入队/跳过 HTTP");
            /* 仍尝试取样入队 */
        }
        report_one_ble((uint8_t)i, cycle_ref_ms, deadline);
    }
}
