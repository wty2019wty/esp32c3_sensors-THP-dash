/*
 * THP 多设备固件 boot
 *
 * 源 0 = 本机 I2C；源 1..N = ATC/BTHome BLE（thp_config.h 设备表）。
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

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
    ESP_LOGI(TAG, "已启动调度 thp_cycle（local%s）",
             thp_ble_is_ready() ? " + BLE multi" : "");
}
