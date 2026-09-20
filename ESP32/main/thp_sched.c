#include "thp_sched.h"

#include "esp_log.h"
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
#ifndef THP_MI_SCAN_OPEN_BEFORE_MS
#define THP_MI_SCAN_OPEN_BEFORE_MS 5000
#endif
#ifndef THP_MI_SCAN_CLOSE_AFTER_MS
#define THP_MI_SCAN_CLOSE_AFTER_MS 10000
#endif
#define THP_FLUSH_MIN_REMAIN_MS 10000

static void stage_time(void)
{
    (void)thp_time_sync_before_report();
}

static void stage_local(TickType_t deadline)
{
    thp_sample_t sample;
    if (!thp_sensors_sample(&sample)) {
        ESP_LOGW(TAG, "本周期无有效本机采样，跳过 LOCAL 上报");
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

    ESP_LOGI(TAG, "采样[LOCAL]%s%s iso=%s → %s (deadline 剩余 %dms)",
             sample.has_th ? " T/H" : "",
             sample.has_p ? " P" : "",
             reading.has_iso ? reading.iso : "(no-ts)", THP_API_BASE,
             (int)thp_deadline_remain_ms(deadline));
    if (sample.has_th) {
        ESP_LOGI(TAG, "  温湿度 T=%.2f°C H=%.2f%%",
                 (double)sample.temperature, (double)sample.humidity);
    }
    if (sample.has_p) {
        ESP_LOGI(TAG, "  气压 P=%.2fhPa", (double)sample.pressure);
    }

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
    /* 窗口已在 T-5s 打开；此处仅以真实 T 作为取帧参考，不再 window_open */
    int64_t cycle_ref_ms = esp_timer_get_time() / 1000;

    thp_report_new_cycle();

    ESP_LOGI(TAG, "—— 周期 #%u 开始 heap=%u wifi=%d queue=%u mi=%d deadline_in=%dms ——",
             cycle_no,
             (unsigned)esp_get_free_heap_size(),
             (int)thp_wifi_is_connected(),
             thp_queue_count(),
             (int)thp_mi_is_ready(),
             (int)thp_deadline_remain_ms(deadline));

    stage_time();
    stage_local(deadline);

    /* BLE 窗口采集到 T+10s 再取距 T 最近的帧 */
    TickType_t win_close = cycle_start_tick + pdMS_TO_TICKS(THP_MI_SCAN_CLOSE_AFTER_MS);
    sleep_until_tick(win_close);

    thp_mi_report_cycle(cycle_ref_ms, deadline);
    thp_mi_scan_window_close();

    stage_flush(deadline);

    ESP_LOGI(TAG, "—— 周期 #%u 结束 heap=%u queue=%u stack_hwm=%u ——",
             cycle_no,
             (unsigned)esp_get_free_heap_size(),
             thp_queue_count(),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    TickType_t elapsed = xTaskGetTickCount() - cycle_start_tick;
    TickType_t next_start = cycle_start_tick + period_ticks;
    TickType_t next_open = next_start - pdMS_TO_TICKS(THP_MI_SCAN_OPEN_BEFORE_MS);
    TickType_t now = xTaskGetTickCount();

    if ((int32_t)(next_open - now) > 0) {
        TickType_t remain = next_open - now;
        ESP_LOGI(TAG, "本周期耗时 %ums，%ums 后进入下一 BLE 窗",
                 (unsigned)(elapsed * portTICK_PERIOD_MS),
                 (unsigned)(remain * portTICK_PERIOD_MS));
        /* 睡到下一周期 T-5s；开窗在下一轮循环开头 */
        vTaskDelay(remain);
    } else {
        ESP_LOGW(TAG, "本周期耗时 %ums，已越过下一窗开启点，立即开窗",
                 (unsigned)(elapsed * portTICK_PERIOD_MS));
    }
}

static void thp_sched_task(void *arg)
{
    (void)arg;
    unsigned cycle_no = 0;
    TickType_t period_ticks = pdMS_TO_TICKS(THP_REPORT_PERIOD_MS);
    TickType_t open_before = pdMS_TO_TICKS(THP_MI_SCAN_OPEN_BEFORE_MS);

    ESP_LOGI(TAG, "调度启动：period=%dms margin=%dms BLE窗=T-%dms~T+%dms first_delay=%dms",
             THP_REPORT_PERIOD_MS, THP_CYCLE_DEADLINE_MARGIN_MS,
             THP_MI_SCAN_OPEN_BEFORE_MS, THP_MI_SCAN_CLOSE_AFTER_MS,
             THP_REPORT_FIRST_DELAY_MS);

    /* 首周期：first_delay 后到达 T0；在此之前不做持续扫描 */
    vTaskDelay(pdMS_TO_TICKS(THP_REPORT_FIRST_DELAY_MS));
    TickType_t next_start = xTaskGetTickCount();

    while (1) {
        TickType_t open_at = next_start - open_before;
        sleep_until_tick(open_at);

        /* T-5s：开 BLE 窗（仅此窗口内扫描；整周期只开一次，避免清空环缓存） */
        if (thp_mi_is_ready()) {
            TickType_t now = xTaskGetTickCount();
            int64_t est_ref_ms;
            if ((int32_t)(next_start - now) > 0) {
                /* 以目标 T 估算 ref，开窗范围覆盖 [T-5s, T+10s] */
                est_ref_ms = esp_timer_get_time() / 1000 +
                             (int64_t)((next_start - now) * portTICK_PERIOD_MS);
            } else {
                est_ref_ms = esp_timer_get_time() / 1000;
            }
            thp_mi_scan_window_open(est_ref_ms);
        }

        sleep_until_tick(next_start);

        TickType_t cycle_start = xTaskGetTickCount();
        /* 若迟到，以实际起点重对齐下一周期，避免窗相对 T 漂移 */
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
    /* 单任务：TLS/队列无跨任务争用；栈 8K 覆盖 HTTP + JSON（观察 stack_hwm 后再调） */
    BaseType_t ok = xTaskCreate(thp_sched_task, "thp_cycle", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建调度任务失败");
    }
}
