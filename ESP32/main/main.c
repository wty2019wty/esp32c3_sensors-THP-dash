/*
 * THP Dash 设备端 — ESP32-C3
 *
 * deep sleep 模型：唤醒 → Wi-Fi/采样/上报/补传 → 关射频 → deep sleep
 * RTC：离线队列 ~16 条 + 系统时间跨睡眠传递
 *
 * 上报：HTTPS POST {THP_API_BASE}/api/v1/readings
 *   Authorization: Bearer <device_token>
 */
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "i2c_config.h"
#include "thp_config.h"
#include "thp_mi.h"
#include "thp_queue.h"
#include "thp_report.h"
#include "thp_sched.h"
#include "thp_sensors.h"
#include "thp_time.h"
#include "thp_wifi.h"

static const char *TAG = "thp";

#ifndef THP_REPORT_PERIOD_MS
#define THP_REPORT_PERIOD_MS (5 * 60 * 1000)
#endif
/* 唤醒后至少还睡这么久，避免抖动导致几乎不睡 */
#define THP_DEEP_SLEEP_MIN_MS 8000

static void nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

/* 动态调频 + light sleep：仅覆盖唤醒后的活跃段（随后进 deep sleep） */
static void power_management_init(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t cfg = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 10,
        .light_sleep_enable = true,
    };
    esp_err_t err = esp_pm_configure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure 失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "PM: max=%dMHz min=10MHz light_sleep=on", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    }
#endif
}

static void check_local_token(void)
{
    if (strcmp(THP_DEVICE_TOKEN, "thp_replace_me") == 0 ||
        strlen(THP_DEVICE_TOKEN) < 8) {
        ESP_LOGE(TAG, "请先配置 thp_config.h 中的 THP_DEVICE_TOKEN（Dash 生成，明文只显示一次）");
    }
}

static void enter_deep_sleep(uint64_t work_elapsed_us)
{
    int64_t period_us = (int64_t)THP_REPORT_PERIOD_MS * 1000LL;
    int64_t sleep_us = period_us - (int64_t)work_elapsed_us;
    int64_t min_us = (int64_t)THP_DEEP_SLEEP_MIN_MS * 1000LL;
    if (sleep_us < min_us) {
        ESP_LOGW(TAG, "活跃段过长(%lldms)，压缩睡眠到 %dms",
                 (long long)(work_elapsed_us / 1000), THP_DEEP_SLEEP_MIN_MS);
        sleep_us = min_us;
    }

    thp_wifi_radio_off();

    /* 深睡前把 I2C 拉成确定态，减轻从设备在睡眠中挂死 */
    gpio_set_direction(I2C_SDA_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_direction(I2C_SCL_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(I2C_SDA_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode(I2C_SCL_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_level(I2C_SDA_GPIO, 1);
    gpio_set_level(I2C_SCL_GPIO, 1);

    thp_time_rtc_save(sleep_us);

    ESP_LOGI(TAG, "进入 deep sleep %lldms（周期 %dms，活跃 %lldms）",
             (long long)(sleep_us / 1000), THP_REPORT_PERIOD_MS,
             (long long)(work_elapsed_us / 1000));

    /* 给串口冲出去一点时间 */
    fflush(stdout);
    esp_sleep_enable_timer_wakeup((uint64_t)sleep_us);
    esp_deep_sleep_start();
}

void app_main(void)
{
    const bool from_sleep = thp_sched_wake_from_sleep();
    ESP_LOGI(TAG, "THP Dash 设备端启动  %s  period=%dms  base=%s",
             from_sleep ? "deep-sleep 唤醒" : "上电/复位",
             THP_REPORT_PERIOD_MS, THP_API_BASE);
    check_local_token();

    nvs_init();
    power_management_init();

    if (from_sleep) {
        thp_time_rtc_restore();
    }

    if (thp_sensors_init() != ESP_OK) {
        ESP_LOGE(TAG, "传感器总线初始化失败");
    }
    if (thp_queue_init() != ESP_OK) {
        ESP_LOGE(TAG, "离线队列初始化失败");
    }
    if (thp_report_init() != ESP_OK) {
        ESP_LOGE(TAG, "上报网络锁创建失败");
    }

    int64_t t0 = esp_timer_get_time();

    if (thp_wifi_init_sta() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 未就绪，本唤醒采样入离线队列");
    }

    /* 上电：完整校时 + 连通性探测；timer 唤醒：只 ensure SNTP，省 probe */
    if (!from_sleep) {
        thp_time_sntp_start();
        thp_report_probe_api();
    } else {
        thp_time_sntp_ensure();
    }

    (void)thp_mi_init();

    thp_sched_run_once();

    int64_t elapsed_us = esp_timer_get_time() - t0;
    enter_deep_sleep(elapsed_us > 0 ? (uint64_t)elapsed_us : 0);
}
