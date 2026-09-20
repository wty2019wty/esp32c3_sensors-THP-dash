#include "thp_sched.h"

#include "esp_log.h"
#include "esp_system.h"
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
#ifndef THP_OFFLINE_FLUSH_MAX_PER_CYCLE
#define THP_OFFLINE_FLUSH_MAX_PER_CYCLE 24
#endif

static void stage_time(void)
{
    (void)thp_time_sync_before_report();
}

static void stage_flush(void)
{
    thp_report_flush_queue(THP_OFFLINE_FLUSH_MAX_PER_CYCLE);
}

static void stage_local(void)
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

    thp_report_or_enqueue(&reading);
}

static void stage_mi(void)
{
    thp_mi_report_cycle();
}

static void run_one_cycle(unsigned cycle_no)
{
    ESP_LOGI(TAG, "—— 周期 #%u 开始 heap=%u wifi=%d queue=%u mi=%d ——",
             cycle_no,
             (unsigned)esp_get_free_heap_size(),
             (int)thp_wifi_is_connected(),
             thp_queue_count(),
             (int)thp_mi_is_ready());

    stage_time();
    stage_flush();
    stage_local();
    stage_mi();

    ESP_LOGI(TAG, "—— 周期 #%u 结束 heap=%u queue=%u ——",
             cycle_no,
             (unsigned)esp_get_free_heap_size(),
             thp_queue_count());
}

static void thp_sched_task(void *arg)
{
    (void)arg;
    unsigned cycle_no = 0;

    ESP_LOGI(TAG, "调度启动：period=%dms flush_max=%d first_delay=%dms",
             THP_REPORT_PERIOD_MS, THP_OFFLINE_FLUSH_MAX_PER_CYCLE,
             THP_REPORT_FIRST_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(THP_REPORT_FIRST_DELAY_MS));

    while (1) {
        cycle_no++;
        TickType_t cycle_start = xTaskGetTickCount();
        run_one_cycle(cycle_no);

        TickType_t elapsed = xTaskGetTickCount() - cycle_start;
        TickType_t period_ticks = pdMS_TO_TICKS(THP_REPORT_PERIOD_MS);
        if (elapsed < period_ticks) {
            TickType_t remain = period_ticks - elapsed;
            ESP_LOGI(TAG, "本周期耗时 %ums，%ums 后进入下一周期",
                     (unsigned)(elapsed * portTICK_PERIOD_MS),
                     (unsigned)(remain * portTICK_PERIOD_MS));
            vTaskDelay(remain);
        } else {
            ESP_LOGW(TAG, "本周期超时耗时 %ums（> period %dms），立即进入下一周期",
                     (unsigned)(elapsed * portTICK_PERIOD_MS),
                     THP_REPORT_PERIOD_MS);
        }
    }
}

void thp_sched_start_task(void)
{
    /* 单任务：TLS/队列无跨任务争用；栈 8K 覆盖 HTTP + JSON */
    BaseType_t ok = xTaskCreate(thp_sched_task, "thp_cycle", 8192, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "创建调度任务失败");
    }
}
