#include "thp_time.h"

#include <string.h>
#include <sys/time.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "thp_config.h"
#include "thp_wifi.h"

static const char *TAG = "thp.time";

#define SNTP_SYNC_BIT BIT0

#ifndef THP_NTP_NUM_SERVERS
#define THP_NTP_NUM_SERVERS 1
#endif
#ifndef THP_NTP_SYNC_TIMEOUT_MS
#define THP_NTP_SYNC_TIMEOUT_MS 20000
#endif
#ifndef THP_NTP_RESYNC_EVERY_CYCLES
#define THP_NTP_RESYNC_EVERY_CYCLES 12
#endif
#ifndef THP_NTP_SERVER_LIST
#ifdef THP_NTP_SERVER
#define THP_NTP_SERVER_LIST ESP_SNTP_SERVER_LIST(THP_NTP_SERVER)
#else
#define THP_NTP_SERVER_LIST ESP_SNTP_SERVER_LIST("pool.ntp.org")
#endif
#endif

static EventGroupHandle_t s_time_events;
/*
 * 已同步后跳过的周期数；达到 THP_NTP_RESYNC_EVERY_CYCLES 则重同步。
 * 必须放 RTC：deep sleep 每次唤醒 RAM 清零，否则永远凑不满 N 次。
 */
RTC_DATA_ATTR static unsigned s_sync_skip_count;

/* deep sleep 跨重启时间传递（RTC 慢速内存） */
RTC_DATA_ATTR static int64_t s_rtc_epoch_us;
RTC_DATA_ATTR static int64_t s_rtc_planned_sleep_us;
RTC_DATA_ATTR static uint32_t s_rtc_time_magic;

#define THP_TIME_MAGIC 0x54485431u /* "THT1" */

static void time_events_ensure(void)
{
    if (s_time_events == NULL) {
        s_time_events = xEventGroupCreate();
    }
}

bool thp_time_is_synced(void)
{
    time_events_ensure();
    if (s_time_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_time_events) & SNTP_SYNC_BIT) != 0;
}

void thp_time_sntp_start(void)
{
    time_events_ensure();

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        THP_NTP_NUM_SERVERS, THP_NTP_SERVER_LIST);
    cfg.start = true;
    cfg.wait_for_sync = true;
    cfg.server_from_dhcp = false;
    cfg.renew_servers_after_new_IP = false;
    cfg.smooth_sync = false;

    if (cfg.num_of_servers > CONFIG_LWIP_SNTP_MAX_SERVERS) {
        ESP_LOGW(TAG, "NTP 服务器数 %u 超过 CONFIG_LWIP_SNTP_MAX_SERVERS=%d，将截断",
                 (unsigned)cfg.num_of_servers, CONFIG_LWIP_SNTP_MAX_SERVERS);
        cfg.num_of_servers = CONFIG_LWIP_SNTP_MAX_SERVERS;
    }

    ESP_LOGI(TAG, "NTP 服务器 %u 个：", (unsigned)cfg.num_of_servers);
    for (size_t i = 0; i < cfg.num_of_servers && i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++) {
        ESP_LOGI(TAG, "  [%u] %s", (unsigned)i, cfg.servers[i] ? cfg.servers[i] : "(null)");
    }

    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP 初始化失败: %s", esp_err_to_name(err));
        return;
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(THP_NTP_SYNC_TIMEOUT_MS));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "启动时 SNTP 时间已同步");
        time_events_ensure();
        if (s_time_events) {
            xEventGroupSetBits(s_time_events, SNTP_SYNC_BIT);
        }
        s_sync_skip_count = 0;
    } else {
        ESP_LOGW(TAG, "启动时 SNTP 同步失败/超时(%s)，将由每周期上报前再同步",
                 esp_err_to_name(err));
    }
}

void thp_time_sntp_ensure(void)
{
    time_events_ensure();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        THP_NTP_NUM_SERVERS, THP_NTP_SERVER_LIST);
    cfg.start = false;
    cfg.wait_for_sync = false;
    cfg.server_from_dhcp = false;
    cfg.renew_servers_after_new_IP = false;
    cfg.smooth_sync = false;
    if (cfg.num_of_servers > CONFIG_LWIP_SNTP_MAX_SERVERS) {
        cfg.num_of_servers = CONFIG_LWIP_SNTP_MAX_SERVERS;
    }
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP ensure init: %s", esp_err_to_name(err));
    }
}

bool thp_time_format_iso_at(time_t now, char *out)
{
    if (out == NULL || now < 1600000000) {
        return false;
    }
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);

    int year = tm_utc.tm_year + 1900;
    if (year < 2000) {
        year = 2000;
    } else if (year > 2200) {
        year = 2200;
    }
    int mon = tm_utc.tm_mon + 1;
    if (mon < 1) {
        mon = 1;
    } else if (mon > 12) {
        mon = 12;
    }
    int day = tm_utc.tm_mday;
    if (day < 1) {
        day = 1;
    } else if (day > 31) {
        day = 31;
    }
    int hour = tm_utc.tm_hour;
    int min = tm_utc.tm_min;
    int sec = tm_utc.tm_sec;
    if (hour < 0 || hour > 23) {
        hour = 0;
    }
    if (min < 0 || min > 59) {
        min = 0;
    }
    if (sec < 0 || sec > 60) {
        sec = 0;
    }

    snprintf(out, ISO_UTC_BUF_LEN, "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
             year, mon, day, hour, min, sec);
    return out[0] != '\0';
}

bool thp_time_format_iso(char *out)
{
    if (out == NULL) {
        return false;
    }
    out[0] = '\0';
    time_t now = 0;
    struct timeval tv = {0};
    time(&now);
    gettimeofday(&tv, NULL);
    if (now < 1600000000) {
        return false;
    }
    if (!thp_time_is_synced()) {
        return false;
    }
    if (!thp_time_format_iso_at(now, out)) {
        return false;
    }
    int ms = (int)(tv.tv_usec / 1000);
    if (ms < 0) {
        ms = 0;
    } else if (ms > 999) {
        ms = 999;
    }
    size_t len = strlen(out);
    if (len >= 5) {
        snprintf(out + len - 5, 6, ".%03dZ", ms);
    }
    return out[0] != '\0';
}

bool thp_time_sync_before_report(void)
{
    time_events_ensure();

    if (!thp_wifi_is_connected()) {
        ESP_LOGW(TAG, "上报前 NTP：Wi-Fi 未连接，跳过同步");
        return thp_time_is_synced();
    }

    /* 一期策略：已同步则短路，每 N 周期才真正重同步（漂移阈值二期） */
    if (thp_time_is_synced() &&
        s_sync_skip_count + 1 < (unsigned)THP_NTP_RESYNC_EVERY_CYCLES) {
        s_sync_skip_count++;
        return true;
    }

    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_start/restart: %s", esp_err_to_name(err));
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(THP_NTP_SYNC_TIMEOUT_MS));
    if (err == ESP_OK) {
        /* 仅在真正同步成功时置位；ESP_ERR_NOT_FINISHED 表示仍在进行中 */
        if (s_time_events) {
            xEventGroupSetBits(s_time_events, SNTP_SYNC_BIT);
        }
        s_sync_skip_count = 0;
        char iso[ISO_UTC_BUF_LEN];
        if (thp_time_format_iso(iso)) {
            ESP_LOGI(TAG, "上报前 NTP 同步 OK  UTC=%s", iso);
        } else {
            ESP_LOGI(TAG, "上报前 NTP 同步 OK");
        }
        return true;
    }

    time_t now = 0;
    time(&now);
    const bool had_sync = thp_time_is_synced();

    if (err == ESP_ERR_NOT_FINISHED) {
        /* 同步进行中：不得把 SYNC_BIT 当成功；已有可信时间则沿用 */
        if (had_sync && now > 1600000000) {
            /* 本周期已真正 sync_wait 过；复位 skip，避免下一周期立刻再打 20s */
            s_sync_skip_count = 0;
            ESP_LOGW(TAG, "上报前 NTP 仍在同步中，沿用已有系统时间");
            return true;
        }
        ESP_LOGW(TAG, "上报前 NTP 仍在同步中且尚无可信时间，本条可能不带 measured_at");
        return false;
    }

    if (had_sync && now > 1600000000) {
        char iso[ISO_UTC_BUF_LEN];
        thp_time_format_iso(iso);
        /* 失败也复位 skip：等价于「失败一次，再隔 RESYNC 周期才重试」 */
        s_sync_skip_count = 0;
        ESP_LOGW(TAG, "上报前 NTP 超时(%s)，沿用已有系统时间 UTC=%s",
                 esp_err_to_name(err), iso[0] ? iso : "(n/a)");
        return true;
    }

    ESP_LOGW(TAG, "上报前 NTP 同步失败(%s)，本条可能不带 measured_at",
             esp_err_to_name(err));
    return false;
}

void thp_time_stamp_reading(thp_reading_t *r)
{
    if (r == NULL) {
        return;
    }
    memset(r, 0, sizeof(*r));
    int rssi = 0;
    if (thp_wifi_get_rssi(&rssi)) {
        r->rssi = rssi;
    }
    if (thp_time_is_synced()) {
        r->has_iso = thp_time_format_iso(r->iso);
    }
}

void thp_time_rtc_save(int64_t planned_sleep_us)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    int64_t epoch_us = (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
    if (epoch_us < 1600000000LL * 1000000LL) {
        /* 尚无可信 UTC，不写 magic，醒来重新 NTP */
        s_rtc_time_magic = 0;
        ESP_LOGW(TAG, "RTC 存时跳过：系统时间尚不可信");
        return;
    }
    s_rtc_epoch_us = epoch_us;
    s_rtc_planned_sleep_us = planned_sleep_us > 0 ? planned_sleep_us : 0;
    s_rtc_time_magic = THP_TIME_MAGIC;
    ESP_LOGI(TAG, "RTC 已存时 epoch_us=%lld planned_sleep_us=%lld",
             (long long)epoch_us, (long long)s_rtc_planned_sleep_us);
}

bool thp_time_rtc_restore(void)
{
    if (s_rtc_time_magic != THP_TIME_MAGIC) {
        ESP_LOGW(TAG, "RTC 无有效时间，待 NTP");
        return false;
    }
    int64_t now_us = s_rtc_epoch_us + s_rtc_planned_sleep_us;
    if (now_us < 1600000000LL * 1000000LL) {
        return false;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(now_us / 1000000LL),
        .tv_usec = (suseconds_t)(now_us % 1000000LL),
    };
    settimeofday(&tv, NULL);
    time_events_ensure();
    if (s_time_events) {
        xEventGroupSetBits(s_time_events, SNTP_SYNC_BIT);
    }
    /* 不重置 s_sync_skip_count：跨唤醒累加，才能按 N 次周期真正打 NTP */
    char iso[ISO_UTC_BUF_LEN];
    thp_time_format_iso(iso);
    ESP_LOGI(TAG, "RTC 恢复系统时间 UTC=%s（含计划睡眠漂移）", iso[0] ? iso : "(fmt-fail)");
    return true;
}
