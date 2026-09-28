#include "thp_sched.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "thp_ble.h"
#include "thp_config.h"
#include "thp_local.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_sources.h"
#include "thp_time.h"
#include "thp_types.h"
#include "thp_wifi.h"

static const char *TAG = "thp.sched";

#ifndef THP_REPORT_FIRST_DELAY_MS
#define THP_REPORT_FIRST_DELAY_MS 5000
#endif
#ifndef THP_REPORT_PERIOD_MS
#define THP_REPORT_PERIOD_MS (5 * 60 * 1000)
#endif
#ifndef THP_CYCLE_DEADLINE_MARGIN_MS
#define THP_CYCLE_DEADLINE_MARGIN_MS 20000
#endif
#ifndef THP_OFFLINE_FLUSH_MAX_PER_CYCLE
#define THP_OFFLINE_FLUSH_MAX_PER_CYCLE 24
#endif
#ifndef THP_BLE_SCAN_OPEN_BEFORE_MS
#define THP_BLE_SCAN_OPEN_BEFORE_MS 5000
#endif
#ifndef THP_BLE_SCAN_CLOSE_AFTER_MS
#define THP_BLE_SCAN_CLOSE_AFTER_MS 10000
#endif
#define THP_FLUSH_MIN_REMAIN_MS 10000

/**
 * LOCAL：T 时刻只采样不发 HTTP，避免 TLS 重试占满 BLE 窗并 stop_scan。
 * 组帧放到窗口关闭后 stage_local_report。
 */
static bool stage_local_sample(thp_sample_t *out)
{
    if (!thp_source_ready(0)) {
        ESP_LOGW(TAG, "LOCAL 源未就绪，跳过本周期采样");
        return false;
    }
    if (!thp_local_sample(out)) {
        ESP_LOGW(TAG, "本周期无有效本机采样，跳过 LOCAL");
        return false;
    }
    ESP_LOGI(TAG, "采样[local]%s%s（HTTP 留到 BLE 窗结束后）",
             out->has_th ? " T/H" : "",
             out->has_p ? " P" : "");
    return true;
}

static void stage_local_report(const thp_sample_t *sample, TickType_t deadline)
{
    if (sample == NULL || !sample->valid) {
        return;
    }
    thp_reading_t reading;
    thp_time_stamp_reading(&reading);
    reading.source_id = 0;
    reading.temperature = sample->temperature;
    reading.humidity = sample->humidity;
    reading.pressure = sample->pressure;
    reading.has_th = sample->has_th;
    reading.has_p = sample->has_p;
    thp_time_fill_sample_iso(&reading, sample->sample_mono_ms);

    ESP_LOGI(TAG, "上报[local] iso=%s (deadline 剩余 %dms)",
             reading.has_iso ? reading.iso : "(no-ts)",
             (int)thp_deadline_remain_ms(deadline));
    thp_report_or_enqueue(&reading, deadline);
}

static void stage_flush(TickType_t deadline)
{
    if (thp_deadline_reached(deadline) ||
        thp_deadline_remain_ms(deadline) < THP_FLUSH_MIN_REMAIN_MS) {
        ESP_LOGW(TAG, "跳过补传：剩余预算 %dms < %dms",
                 (int)thp_deadline_remain_ms(deadline), THP_FLUSH_MIN_REMAIN_MS);
        return;
    }
    thp_report_flush_queue(THP_OFFLINE_FLUSH_MAX_PER_CYCLE, deadline);
}

static TickType_t cycle_deadline(TickType_t cycle_start)
{
    TickType_t period_ticks = pdMS_TO_TICKS(THP_REPORT_PERIOD_MS);
    TickType_t margin = pdMS_TO_TICKS(THP_CYCLE_DEADLINE_MARGIN_MS);
    if (period_ticks <= margin) {
        return cycle_start + period_ticks;
    }
    return cycle_start + (period_ticks - margin);
}

static void sleep_until_tick(TickType_t target)
{
    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(target - now) > 0) {
        vTaskDelay(target - now);
    }
}

static void run_one_cycle(unsigned cycle_no, TickType_t cycle_start_tick)
{
    TickType_t deadline = cycle_deadline(cycle_start_tick);
    TickType_t period_ticks = pdMS_TO_TICKS(THP_REPORT_PERIOD_MS);
    int64_t cycle_ref_ms = esp_timer_get_time() / 1000;
    thp_ble_scan_window_realign(cycle_ref_ms);

    thp_report_new_cycle();

    ESP_LOGI(TAG, "—— 周期 #%u 开始 heap=%u wifi=%d queue=%u ble=%d deadline_in=%dms ——",
             cycle_no,
             (unsigned)esp_get_free_heap_size(),
             (int)thp_wifi_is_connected(),
             thp_queue_count(),
             (int)thp_ble_is_ready(),
             (int)thp_deadline_remain_ms(deadline));

    /* 窗内禁止 HTTP/NTP：只采样 + 扫 BLE，避免 stop_scan/重试饿死窗口 */
    thp_sample_t local_sample;
    bool has_local = stage_local_sample(&local_sample);

    if (thp_ble_is_ready()) {
        TickType_t win_close = cycle_start_tick + pdMS_TO_TICKS(THP_BLE_SCAN_CLOSE_AFTER_MS);
        sleep_until_tick(win_close);
        thp_ble_scan_window_close();
    }

    /* 窗口已关，再校时并串行上报（LOCAL → 各 BLE → 补传） */
    (void)thp_time_sync_before_report();
    if (has_local) {
        stage_local_report(&local_sample, deadline);
    }
    thp_ble_report_cycle(cycle_ref_ms, deadline);

    stage_flush(deadline);

    ESP_LOGI(TAG, "—— 周期 #%u 结束 heap=%u queue=%u stack_hwm=%u ——",
             cycle_no,
             (unsigned)esp_get_free_heap_size(),
             thp_queue_count(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    TickType_t elapsed = xTaskGetTickCount() - cycle_start_tick;
    TickType_t next_start = cycle_start_tick + period_ticks;
    TickType_t next_open = next_start - pdMS_TO_TICKS(THP_BLE_SCAN_OPEN_BEFORE_MS);
    TickType_t now = xTaskGetTickCount();

    if ((int32_t)(next_open - now) > 0) {
        vTaskDelay(next_open - now);
    } else {
        ESP_LOGW(TAG, "本周期耗时 %ums，已越过下一窗开启点",
                 (unsigned)(elapsed * portTICK_PERIOD_MS));
    }
}

static void thp_sched_task(void *arg)
{
    (void)arg;
    unsigned cycle_no = 0;
    TickType_t period_ticks = pdMS_TO_TICKS(THP_REPORT_PERIOD_MS);
    TickType_t open_before = pdMS_TO_TICKS(THP_BLE_SCAN_OPEN_BEFORE_MS);

    ESP_LOGI(TAG, "调度启动：period=%dms margin=%dms BLE窗=T-%dms~T+%dms",
             THP_REPORT_PERIOD_MS, THP_CYCLE_DEADLINE_MARGIN_MS,
             THP_BLE_SCAN_OPEN_BEFORE_MS, THP_BLE_SCAN_CLOSE_AFTER_MS);

    vTaskDelay(pdMS_TO_TICKS(THP_REPORT_FIRST_DELAY_MS));
    TickType_t next_start = xTaskGetTickCount();

    while (1) {
        TickType_t open_at = next_start - open_before;
        sleep_until_tick(open_at);

        if (thp_ble_is_ready()) {
            TickType_t now = xTaskGetTickCount();
            int64_t est_ref_ms;
            if ((int32_t)(next_start - now) > 0) {
                est_ref_ms = esp_timer_get_time() / 1000 +
                             (int64_t)((next_start - now) * portTICK_PERIOD_MS);
            } else {
                est_ref_ms = esp_timer_get_time() / 1000;
            }
            thp_ble_scan_window_open(est_ref_ms);
        }

        sleep_until_tick(next_start);

        TickType_t cycle_start = xTaskGetTickCount();
        if ((int32_t)(cycle_start - next_start) > pdMS_TO_TICKS(1000)) {
            next_start = cycle_start;
        }

        cycle_no++;
        run_one_cycle(cycle_no, next_start);
        next_start = next_start + period_ticks;
    }
}

void thp_sched_start_task(void)
{
    /* TLS 握手 + 证书链校验栈耗较高，8KB 易溢出导致 mbedtls -0x7200 */
    BaseType_t ok = xTaskCreate(thp_sched_task, "thp_cycle", 16384, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建调度任务失败");
    }
}
