#include "thp_sched.h"

#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "thp_config.h"
#include "thp_mi.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_sensors.h"
#include "thp_time.h"
#include "thp_types.h"
#include "thp_wifi.h"

static const char *TAG = "thp.sched";

#ifndef THP_OFFLINE_FLUSH_MAX_PER_CYCLE
#define THP_OFFLINE_FLUSH_MAX_PER_CYCLE 16
#endif
#define THP_FLUSH_MIN_REMAIN_MS 8000

bool thp_sched_wake_from_sleep(void)
{
    /* IDF 6：causes 为 bit mask */
    return (esp_sleep_get_wakeup_causes() & (1u << ESP_SLEEP_WAKEUP_TIMER)) != 0;
}

static void stage_time(void)
{
    (void)thp_time_sync_before_report();
}

static void stage_local(void)
{
    thp_sample_t sample;
    if (!thp_sensors_sample(&sample)) {
        ESP_LOGW(TAG, "本唤醒无有效本机采样，跳过 LOCAL 上报");
        return;
    }

    thp_reading_t reading;
    thp_time_stamp_reading(&reading);
    reading.kind = THP_KIND_LOCAL;
    reading.temperature = sample.temperature;
    reading.humidity = sample.humidity;
    reading.pressure = sample.pressure;
    reading.has_th = sample.has_th;
    reading.has_p = sample.has_p;

    ESP_LOGI(TAG, "采样[LOCAL]%s%s iso=%s → %s",
             sample.has_th ? " T/H" : "",
             sample.has_p ? " P" : "",
             reading.has_iso ? reading.iso : "(no-ts)", THP_API_BASE);
    if (sample.has_th) {
        ESP_LOGI(TAG, "  温湿度 T=%.2f°C H=%.2f%%",
                 (double)sample.temperature, (double)sample.humidity);
    }
    if (sample.has_p) {
        ESP_LOGI(TAG, "  气压 P=%.2fhPa", (double)sample.pressure);
    }

    /* deadline 用当前 tick + 剩余预算（唤醒内一次做完，不跨周期） */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(THP_REPORT_MAX_RETRIES * THP_HTTP_TIMEOUT_MS + 8000);
    thp_report_or_enqueue(&reading, deadline);
}

static void stage_mi(void)
{
    if (!thp_mi_is_ready()) {
        return;
    }
    int64_t ref_ms = esp_timer_get_time() / 1000;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(THP_HTTP_TIMEOUT_MS + 4000);
    thp_mi_scan_window_realign(ref_ms);
    thp_mi_report_cycle(ref_ms, deadline);
    thp_mi_scan_window_close();
}

static void stage_flush(void)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(THP_HTTP_TIMEOUT_MS + 4000);
    unsigned pending = thp_queue_count();
    if (pending == 0) {
        return;
    }
    /* 有积压时再给一些预算；仍受 report 内 deadline 约束 */
    deadline = xTaskGetTickCount() +
               pdMS_TO_TICKS((THP_HTTP_TIMEOUT_MS + 2000) * (THP_OFFLINE_FLUSH_MAX_PER_CYCLE + 2));
    ESP_LOGI(TAG, "补传预算内处理积压 %u 条（上限 %d）",
             pending, THP_OFFLINE_FLUSH_MAX_PER_CYCLE);
    thp_report_flush_queue(THP_OFFLINE_FLUSH_MAX_PER_CYCLE, deadline);
}

void thp_sched_run_once(void)
{
    thp_report_new_cycle();

    ESP_LOGI(TAG, "—— 唤醒工作开始 heap=%u wifi=%d queue=%u ——",
             (unsigned)esp_get_free_heap_size(),
             (int)thp_wifi_is_connected(),
             thp_queue_count());

    stage_time();
    stage_local();
    stage_mi();
    stage_flush();

    ESP_LOGI(TAG, "—— 唤醒工作结束 heap=%u queue=%u ——",
             (unsigned)esp_get_free_heap_size(),
             thp_queue_count());
}
