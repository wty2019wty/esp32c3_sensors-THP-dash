/*
 * THP 多设备固件 boot
 *
 * 源 0 = 本机 I2C；源 1..N = ATC/BTHome BLE（thp_config.h 设备表）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "atc_ble.h"
#include "thp_ble.h"
#include "thp_config.h"
#include "thp_local.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_sched.h"
#include "thp_sources.h"
#include "thp_time.h"
#include "thp_wifi.h"

static const char *TAG = "thp";

#ifndef THP_RSSI_DEBUG
#define THP_RSSI_DEBUG 0
#endif
#ifndef THP_RSSI_DEBUG_INTERVAL_MS
#define THP_RSSI_DEBUG_INTERVAL_MS 2000
#endif
#ifndef THP_RSSI_DEBUG_HOLD_SCAN
#define THP_RSSI_DEBUG_HOLD_SCAN 1
#endif

#if THP_RSSI_DEBUG
static void rssi_dbg_task(void *arg)
{
    (void)arg;
#if THP_RSSI_DEBUG_HOLD_SCAN
    atc_ble_set_hold_scan(true);
#endif
    ESP_LOGI(TAG, "RSSI 调试启动 interval=%dms hold_scan=%d",
             THP_RSSI_DEBUG_INTERVAL_MS, THP_RSSI_DEBUG_HOLD_SCAN);

    for (;;) {
        int wifi_rssi = 0;
        bool wifi_ok = thp_wifi_get_rssi(&wifi_rssi);
        char wifi_part[32];
        if (wifi_ok) {
            snprintf(wifi_part, sizeof(wifi_part), "%ddBm", wifi_rssi);
        } else {
            snprintf(wifi_part, sizeof(wifi_part), "--(%d)", (int)thp_wifi_is_connected());
        }

        char ble_part[256];
        int off = 0;
        ble_part[0] = '\0';
        const size_t n = thp_source_count();
        for (size_t i = 0; i < n; i++) {
            if (thp_source_kind((uint8_t)i) != THP_SRC_BLE) {
                continue;
            }
            int di = thp_source_ble_index((uint8_t)i);
            if (di < 0) {
                continue;
            }
            if (off > 0 && off < (int)sizeof(ble_part) - 1) {
                ble_part[off++] = ' ';
                ble_part[off] = '\0';
            }
            atc_ble_sample_t s;
            int w;
            if (atc_ble_pop_latest((size_t)di, &s) && s.valid) {
                long long age = (long long)(esp_timer_get_time() / 1000 - s.ts_ms);
                if (age < 0) {
                    age = 0;
                }
                w = snprintf(ble_part + off, sizeof(ble_part) - (size_t)off,
                             "%s:%ddBm(%llds)",
                             thp_source_name((uint8_t)i), (int)s.rssi, age / 1000);
            } else {
                w = snprintf(ble_part + off, sizeof(ble_part) - (size_t)off,
                             "%s:--", thp_source_name((uint8_t)i));
            }
            if (w < 0) {
                break;
            }
            off += w;
            if (off >= (int)sizeof(ble_part)) {
                off = (int)sizeof(ble_part) - 1;
                break;
            }
        }

        ESP_LOGI(TAG, "[RSSI] wifi=%s | ble=[%s] | scan=%d",
                 wifi_part,
                 ble_part[0] ? ble_part : "none",
                 (int)atc_ble_is_scanning());
        vTaskDelay(pdMS_TO_TICKS(THP_RSSI_DEBUG_INTERVAL_MS));
    }
}

static void thp_rssi_dbg_start(void)
{
    BaseType_t ok = xTaskCreate(rssi_dbg_task, "thp_rssi", 3072, NULL, 3, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "RSSI 调试任务创建失败");
    }
}
#else
static void thp_rssi_dbg_start(void) {}
#endif

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "THP 多设备固件启动  period=%dms  base=%s",
             THP_REPORT_PERIOD_MS, THP_API_BASE);

    nvs_init();

    if (thp_sources_init() != ESP_OK) {
        ESP_LOGE(TAG, "源表初始化失败");
    }

    if (thp_local_init() != ESP_OK) {
        ESP_LOGE(TAG, "本机传感器总线初始化失败");
    }

    if (thp_queue_init() != ESP_OK) {
        ESP_LOGE(TAG, "离线队列互斥锁创建失败");
    }
    if (thp_report_init() != ESP_OK) {
        ESP_LOGE(TAG, "上报网络锁创建失败");
    }

    if (thp_wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 首次未就绪，调度任务仍启动，断网读数入离线队列");
    }

    thp_time_sntp_start();
    thp_report_probe_api();

    thp_sched_start_task();
    thp_rssi_dbg_start();
    ESP_LOGI(TAG, "已启动调度 thp_cycle（local%s）",
             thp_ble_is_ready() ? " + BLE multi" : "");
}
