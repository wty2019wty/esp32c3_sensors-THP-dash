/*
 * THP Dash 设备端 — ESP32-C3
 *
 * 模块：
 *   thp_wifi / thp_time / thp_sensors / thp_queue / thp_report / thp_mi
 *   thp_sched — 单任务周期流水线
 *               BLE 窗 T-5s~T+10s（默认不持续扫描）
 *               校时 → LOCAL → 关窗取 MI → 补传 → 睡到下窗
 *               deadline = period - margin，耗尽时 HTTP 跳过、读数仍入队
 *
 * 上报：HTTPS POST {THP_API_BASE}/api/v1/readings
 *   Authorization: Bearer <device_token>
 */
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "thp_config.h"
#include "thp_mi.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_sched.h"
#include "thp_sensors.h"
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

static void check_local_token(void)
{
    if (strcmp(THP_DEVICE_TOKEN, "thp_replace_me") == 0 ||
        strlen(THP_DEVICE_TOKEN) < 8) {
        ESP_LOGE(TAG, "请先配置 thp_config.h 中的 THP_DEVICE_TOKEN（Dash 生成，明文只显示一次）");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "THP Dash 设备端启动  period=%dms  base=%s",
             THP_REPORT_PERIOD_MS, THP_API_BASE);
    check_local_token();

    nvs_init();

    if (thp_sensors_init() != ESP_OK) {
        ESP_LOGE(TAG, "传感器总线初始化失败");
    }

    if (thp_queue_init() != ESP_OK) {
        ESP_LOGE(TAG, "离线队列互斥锁创建失败");
    }
    if (thp_report_init() != ESP_OK) {
        ESP_LOGE(TAG, "上报网络锁创建失败");
    }

    /* Wi-Fi 首次超时也继续：事件回调后台重连；任务内无网则采样入队 */
    if (thp_wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 首次未就绪，调度任务仍启动，断网读数入离线队列");
    }

    thp_time_sntp_start();
    thp_report_probe_api();

    (void)thp_mi_init();

    thp_sched_start_task();
    ESP_LOGI(TAG, "已启动单周期调度任务 thp_cycle（LOCAL%s）",
             thp_mi_is_ready() ? " + MI" : "");
}
