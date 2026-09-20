/*
 * THP Dash 设备端 — ESP32-C3
 *   SHT40  : 温度 °C、湿度 %RH     （业务 temperature / humidity）
 *   BMP280 : 气压 hPa              （业务 pressure；内部温度仅作 t_fine）
 *
 * 上报：HTTPS POST {THP_API_BASE}/api/v1/readings
 *   Authorization: Bearer <device_token>
 *   Body: temperature, humidity, pressure, measured_at?, device_id?, rssi
 *
 * 需求对齐：REQUIREMENTS.md §4 / §10 / §12
 * 参考驱动：G:\esp32s3\esp32c3_sensors（ESP-IDF 新版 i2c_master）
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/ip_addr.h"
#include "nvs_flash.h"

#include "bmp280.h"
#include "i2c_config.h"
#include "sht40.h"
#include "thp_config.h"
#include "thp_tls_trust.h"
#include "atc_ble.h"

#ifndef THP_MI_ENABLE
#define THP_MI_ENABLE 0
#endif
#ifndef THP_MI_DEVICE_TOKEN
#define THP_MI_DEVICE_TOKEN "thp_replace_me_mi"
#endif
#ifndef THP_MI_DEVICE_ID
#define THP_MI_DEVICE_ID ""
#endif
#ifndef THP_MI_MAC
#define THP_MI_MAC ""
#endif
#ifndef THP_MI_BINDKEY
#define THP_MI_BINDKEY ""
#endif
#ifndef THP_MI_SCAN_BEFORE_REPORT_MS
#define THP_MI_SCAN_BEFORE_REPORT_MS 8000
#endif
#ifndef THP_MI_MAX_AGE_MS
#define THP_MI_MAX_AGE_MS (3 * 5 * 60 * 1000)
#endif

static const char *TAG = "thp";

typedef enum {
    THP_KIND_LOCAL = 0,
    THP_KIND_MI    = 1,
} thp_device_kind_t;

static bool s_mi_ready; /* Token + MAC 配置齐全且 BLE 已 init */

static const char *token_for_kind(thp_device_kind_t k)
{
    return (k == THP_KIND_MI) ? THP_MI_DEVICE_TOKEN : THP_DEVICE_TOKEN;
}

static const char *device_id_for_kind(thp_device_kind_t k)
{
    return (k == THP_KIND_MI) ? THP_MI_DEVICE_ID : THP_DEVICE_ID;
}

static const char *kind_tag(thp_device_kind_t k)
{
    return (k == THP_KIND_MI) ? "MI" : "LOCAL";
}

static bool mi_token_ok(void)
{
    return THP_MI_DEVICE_TOKEN[0] != '\0' &&
           strcmp(THP_MI_DEVICE_TOKEN, "thp_replace_me_mi") != 0 &&
           strlen(THP_MI_DEVICE_TOKEN) >= 8;
}

/* ---------------- Wi-Fi / SNTP ---------------- */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define SNTP_SYNC_BIT       BIT2

#define WIFI_MAX_RETRY      8
#define I2C_GLITCH_IGNORE_CNT 7
#define I2C_TIMEOUT_MS      100
#ifndef THP_HTTP_TIMEOUT_MS
#define THP_HTTP_TIMEOUT_MS 40000
#endif
#define HTTP_RECV_BUF       1024

#ifndef THP_OFFLINE_QUEUE_LEN
#define THP_OFFLINE_QUEUE_LEN 288
#endif
#ifndef THP_OFFLINE_FLUSH_GAP_MS
#define THP_OFFLINE_FLUSH_GAP_MS 200
#endif
#ifndef THP_OFFLINE_FLUSH_MAX_PER_CYCLE
#define THP_OFFLINE_FLUSH_MAX_PER_CYCLE 24
#endif

static EventGroupHandle_t s_wifi_events;
static i2c_master_bus_handle_t s_bus;
static sht40_t s_sht;
static bmp280_t s_bmp;
static int s_wifi_retry;

/* ---------------- I2C ---------------- */

static void i2c_lines_selftest(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << I2C_SDA_GPIO) | (1ULL << I2C_SCL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "I2C 线自检配置失败");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    int sda = gpio_get_level(I2C_SDA_GPIO);
    int scl = gpio_get_level(I2C_SCL_GPIO);
    ESP_LOGI(TAG, "I2C 线自检：SDA=%d SCL=%d (1=可拉高)", sda, scl);
    if (sda == 0 || scl == 0) {
        ESP_LOGW(TAG, "I2C 线无法拉高：检查板载 LED(GPIO8)/短路/上拉电阻");
    }
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_io_num = I2C_SDA_GPIO,
        .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

/* ---------------- Wi-Fi ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_wifi_retry++;
            ESP_LOGW(TAG, "Wi-Fi 断开，重连 %d/%d", s_wifi_retry, WIFI_MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 已连接，IP=" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, THP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, THP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 部分 ESP32-C3 Super Mini 天线差：默认 20 dBm 易 AUTH_EXPIRE / 极不稳定。
     * 参考 esp32c3-ir-web-ESP32-C3：esp_wifi_set_max_tx_power 单位为 0.25 dBm。 */
    int8_t tx_dbm = (int8_t)THP_WIFI_STA_TX_POWER_DBM;
    if (tx_dbm < 8) {
        tx_dbm = 8;
    } else if (tx_dbm > 20) {
        tx_dbm = 20;
    }
    esp_err_t pwr_err = esp_wifi_set_max_tx_power((int8_t)(tx_dbm * 4));
    if (pwr_err != ESP_OK) {
        ESP_LOGW(TAG, "set max TX power (%d dBm) failed: %s", (int)tx_dbm, esp_err_to_name(pwr_err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi STA max TX power = %d dBm", (int)tx_dbm);
    }

    ESP_LOGI(TAG, "Wi-Fi STA 启动，SSID=%s", THP_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(THP_WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "Wi-Fi 连接超时/失败，请检查 SSID/密码/信号");
    return ESP_FAIL;
}

static bool wifi_is_connected(void)
{
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

#ifndef THP_NTP_NUM_SERVERS
#define THP_NTP_NUM_SERVERS 1
#endif
#ifndef THP_NTP_SYNC_TIMEOUT_MS
#define THP_NTP_SYNC_TIMEOUT_MS 20000
#endif
/* 兼容旧配置：仅 THP_NTP_SERVER */
#ifndef THP_NTP_SERVER_LIST
#ifdef THP_NTP_SERVER
#define THP_NTP_SERVER_LIST ESP_SNTP_SERVER_LIST(THP_NTP_SERVER)
#else
#define THP_NTP_SERVER_LIST ESP_SNTP_SERVER_LIST("pool.ntp.org")
#endif
#endif

#define ISO_UTC_BUF_LEN 32

static void sntp_start(void)
{
    /* IDF 6.x：esp_netif_sntp_init(config)；多服务器见 ESP_SNTP_SERVER_LIST */
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
        xEventGroupSetBits(s_wifi_events, SNTP_SYNC_BIT);
    } else {
        ESP_LOGW(TAG, "启动时 SNTP 同步失败/超时(%s)，将由每次上报前再同步",
                 esp_err_to_name(err));
    }
}

/**
 * @brief 将指定 UTC 时刻格式化为 ISO-8601（历史补传用，ms=000）
 */
static bool format_iso_utc_at(time_t now, char *out)
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

/**
 * @brief 生成当前时刻 ISO-8601 UTC，如 2026-01-01T12:00:00.000Z
 * @param[out] out 至少 ISO_UTC_BUF_LEN 字节
 * @return true 时间有效；false 未同步则调用方应省略 measured_at
 */
static bool format_iso_utc(char *out)
{
    if (out == NULL) {
        return false;
    }
    out[0] = '\0';
    time_t now = 0;
    struct timeval tv = {0};
    time(&now);
    gettimeofday(&tv, NULL);
    /* 未校时或系统时钟明显未就绪时不生成 measured_at */
    if (now < 1600000000) {
        return false;
    }
    if ((xEventGroupGetBits(s_wifi_events) & SNTP_SYNC_BIT) == 0) {
        /* 至少成功同步过一次才写 measured_at，避免上电假时间 */
        return false;
    }
    if (!format_iso_utc_at(now, out)) {
        return false;
    }
    int ms = (int)(tv.tv_usec / 1000);
    if (ms < 0) {
        ms = 0;
    } else if (ms > 999) {
        ms = 999;
    }
    /* format_iso_utc_at 以 ".000Z" 结尾；就地改写毫秒 */
    size_t len = strlen(out);
    if (len >= 5) {
        snprintf(out + len - 5, 6, ".%03dZ", ms);
    }
    return out[0] != '\0';
}

/**
 * @brief 每次上报前用多 NTP 服务器同步系统时间
 * @return true 表示可写 measured_at（本次同步成功，或同步失败但已有可信系统时间）
 */
static bool sntp_sync_before_report(void)
{
    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "上报前 NTP：Wi-Fi 未连接，跳过同步");
        return (xEventGroupGetBits(s_wifi_events) & SNTP_SYNC_BIT) != 0;
    }

    /* restart：已 init 则立即重新查询全部配置的 NTP 服务器 */
    esp_err_t err = esp_netif_sntp_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_start/restart: %s", esp_err_to_name(err));
    }

    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(THP_NTP_SYNC_TIMEOUT_MS));
    if (err == ESP_OK || err == ESP_ERR_NOT_FINISHED) {
        xEventGroupSetBits(s_wifi_events, SNTP_SYNC_BIT);
        char iso[ISO_UTC_BUF_LEN];
        if (format_iso_utc(iso)) {
            ESP_LOGI(TAG, "上报前 NTP 同步 OK  UTC=%s (%s)",
                     iso, (err == ESP_OK) ? "synced" : "in-progress");
        } else {
            ESP_LOGI(TAG, "上报前 NTP 同步 OK（%s）", esp_err_to_name(err));
        }
        return true;
    }

    time_t now = 0;
    time(&now);
    const bool had_sync = (xEventGroupGetBits(s_wifi_events) & SNTP_SYNC_BIT) != 0;
    if (had_sync && now > 1600000000) {
        char iso[ISO_UTC_BUF_LEN];
        format_iso_utc(iso);
        ESP_LOGW(TAG, "上报前 NTP 超时(%s)，沿用已有系统时间 UTC=%s",
                 esp_err_to_name(err), iso[0] ? iso : "(n/a)");
        return true;
    }

    ESP_LOGW(TAG, "上报前 NTP 同步失败(%s)，本条可能不带 measured_at",
             esp_err_to_name(err));
    return false;
}

/* ---------------- 传感器采样与业务校验 ---------------- */

typedef struct {
    float temperature;  /* SHT40 */
    float humidity;     /* SHT40 */
    float pressure;     /* BMP280 hPa */
    bool  has_th;       /* 温湿度有效（SHT40） */
    bool  has_p;        /* 气压有效（BMP280） */
    bool  valid;        /* 至少一组字段有效 */
} thp_sample_t;

/** 一帧待上报/待补传读数；iso 为采样时刻 UTC（补传时写入 ts） */
typedef struct {
    thp_device_kind_t kind;
    float temperature;
    float humidity;
    float pressure;
    bool has_th;
    bool has_p;
    int rssi;
    char iso[ISO_UTC_BUF_LEN];
    bool has_iso;
} thp_reading_t;

/** 与 Worker validateReadingPayload 对齐（src/routes/readings.js） */
static bool th_in_range(float t, float h)
{
    return t >= -40.0f && t <= 85.0f && h >= 0.0f && h <= 100.0f;
}

static bool p_in_range(float p)
{
    return p >= 300.0f && p <= 1200.0f;
}

/** 传感器未 present 时尝试重新初始化（热插拔/排线修复后不重启也能恢复） */
static void sensors_retry_init_if_missing(void)
{
    if (!s_sht.present) {
        if (sht40_init(&s_sht, s_bus, I2C_SCL_SPEED_HZ) == ESP_OK) {
            ESP_LOGW(TAG, "SHT40 热修复探测成功，恢复温湿度上报");
        }
    }
    if (!s_bmp.present) {
        if (bmp280_init(&s_bmp, s_bus, I2C_SCL_SPEED_HZ) == ESP_OK) {
            ESP_LOGW(TAG, "BMP280 热修复探测成功，恢复气压上报");
        }
    }
}

/**
 * @brief 采样；允许仅温湿度或仅气压
 * @return true：至少一组字段有效并可上报
 */
static bool sample_read(thp_sample_t *out)
{
    memset(out, 0, sizeof(*out));
    sensors_retry_init_if_missing();

    float sht_t = 0.0f, sht_h = 0.0f;
    float bmp_t = 0.0f, bmp_p = 0.0f, bmp_alt = 0.0f;
    int32_t t_fine = 0;

    bool sht_ok = s_sht.present && (sht40_read(&s_sht, &sht_t, &sht_h) == ESP_OK);
    /* bmp_t 仅用于内部补偿展示；业务温度不使用 BMP280 */
    bool bmp_ok = s_bmp.present && (bmp280_read(&s_bmp, &bmp_t, &bmp_p, &bmp_alt, &t_fine) == ESP_OK);

    if (!sht_ok || !bmp_ok) {
        (void)i2c_master_bus_reset(s_bus);
        /* 总线复位后各再试一次，便于瞬时 NAK 恢复 */
        if (!sht_ok) {
            sht_ok = s_sht.present && (sht40_read(&s_sht, &sht_t, &sht_h) == ESP_OK);
        }
        if (!bmp_ok) {
            bmp_ok = s_bmp.present && (bmp280_read(&s_bmp, &bmp_t, &bmp_p, &bmp_alt, &t_fine) == ESP_OK);
        }
    }

    if (sht_ok && th_in_range(sht_t, sht_h)) {
        out->has_th = true;
        out->temperature = sht_t;
        out->humidity = sht_h;
    } else if (sht_ok) {
        ESP_LOGW(TAG, "温湿度超范围，本帧不带上报: T=%.2f H=%.2f",
                 (double)sht_t, (double)sht_h);
    }

    if (bmp_ok && p_in_range(bmp_p)) {
        out->has_p = true;
        out->pressure = bmp_p;
    } else if (bmp_ok) {
        ESP_LOGW(TAG, "气压超范围，本帧不带上报: P=%.2f", (double)bmp_p);
    }

    out->valid = out->has_th || out->has_p;

    if (!out->valid) {
        ESP_LOGW(TAG, "采样无有效字段: SHT40=%s BMP280=%s present(th=%d p=%d)",
                 sht_ok ? "OK" : "FAIL", bmp_ok ? "OK" : "FAIL",
                 s_sht.present, s_bmp.present);
        return false;
    }

    if (!sht_ok || !bmp_ok) {
        ESP_LOGW(TAG, "部分采样: SHT40=%s BMP280=%s → 上报%s%s",
                 sht_ok ? "OK" : "FAIL", bmp_ok ? "OK" : "FAIL",
                 out->has_th ? " 温湿度" : "", out->has_p ? " 气压" : "");
    }
    return true;
}

/* ---------------- 离线补传队列（RAM 环形缓冲） ---------------- */

static thp_reading_t s_offline_q[THP_OFFLINE_QUEUE_LEN];
static size_t s_offline_head;   /* 最旧一条下标 */
static size_t s_offline_count;

/**
 * @brief 采样时刻打 UTC 戳；仅当至少成功 NTP 过一次且时钟可信时写 iso
 */
static void reading_stamp_now(thp_reading_t *r)
{
    memset(r, 0, sizeof(*r));
    time_t now = 0;
    time(&now);
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        r->rssi = ap.rssi;
    }
    if ((xEventGroupGetBits(s_wifi_events) & SNTP_SYNC_BIT) != 0) {
        r->has_iso = format_iso_utc(r->iso);
    }
}

static void offline_queue_push(const thp_reading_t *r)
{
    if (s_offline_count >= THP_OFFLINE_QUEUE_LEN) {
        /* 队列满：丢最旧，保住较近数据 */
        s_offline_head = (s_offline_head + 1) % THP_OFFLINE_QUEUE_LEN;
        s_offline_count--;
        ESP_LOGW(TAG, "离线队列已满(>%d)，丢弃最旧一条", THP_OFFLINE_QUEUE_LEN);
    }
    size_t idx = (s_offline_head + s_offline_count) % THP_OFFLINE_QUEUE_LEN;
    s_offline_q[idx] = *r;
    s_offline_count++;
    ESP_LOGW(TAG, "已入离线队列 kind=%s (%u/%u) iso=%s%s%s",
             kind_tag(r->kind),
             (unsigned)s_offline_count, (unsigned)THP_OFFLINE_QUEUE_LEN,
             r->has_iso ? r->iso : "(no-ts)",
             r->has_th ? " TH" : "",
             r->has_p ? " P" : "");
}

static const thp_reading_t *offline_queue_peek(void)
{
    if (s_offline_count == 0) {
        return NULL;
    }
    return &s_offline_q[s_offline_head];
}

static void offline_queue_pop(void)
{
    if (s_offline_count == 0) {
        return;
    }
    s_offline_head = (s_offline_head + 1) % THP_OFFLINE_QUEUE_LEN;
    s_offline_count--;
}

static void offline_queue_clear(void)
{
    s_offline_head = 0;
    s_offline_count = 0;
}

/* ---------------- HTTPS 上报 ---------------- */

typedef enum {
    THP_HTTP_OK = 0,
    THP_HTTP_AUTH_FAIL,     /* 401/403：Token 无效/吊销，不重试 */
    THP_HTTP_BAD_PAYLOAD,   /* 400：载荷不合法，不重试 */
    THP_HTTP_TRANSIENT,     /* 网络/5xx：有限重试 */
} thp_http_result_t;

/** 从 THP_API_BASE 提取 host[:port] 与 path 前的 scheme */
static bool split_api_base(char *host, size_t host_n, uint16_t *port, bool *is_https)
{
    const char *base = THP_API_BASE;
    if (strncmp(base, "https://", 8) == 0) {
        *is_https = true;
        base += 8;
        *port = 443;
    } else if (strncmp(base, "http://", 7) == 0) {
        *is_https = false;
        base += 7;
        *port = 80;
    } else {
        return false;
    }
    size_t i = 0;
    while (base[i] && base[i] != '/' && base[i] != ':' && i + 1 < host_n) {
        host[i] = base[i];
        i++;
    }
    host[i] = '\0';
    if (base[i] == ':') {
        long p = strtol(&base[i + 1], NULL, 10);
        if (p > 0 && p < 65536) {
            *port = (uint16_t)p;
        }
    }
    return host[0] != '\0';
}

/**
 * @brief 预检：解析 IPv4 + TCP connect，区分 DNS/路由/TLS 问题
 */
static void probe_api_endpoint(void)
{
    char host[64];
    uint16_t port = 443;
    bool is_https = false;
    if (!split_api_base(host, sizeof(host), &port, &is_https)) {
        ESP_LOGE(TAG, "THP_API_BASE 非法: %s", THP_API_BASE);
        return;
    }

    ESP_LOGI(TAG, "探测 %s  heap_free=%u", THP_API_BASE, (unsigned)esp_get_free_heap_size());

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;          /* 强制 IPv4，避开 AAAA 黑洞 */
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS 解析失败 host=%s gai=%d（检查路由器 DNS / 域名）", host, gai);
        return;
    }

    char ipstr[IPADDR_STRLEN_MAX] = {0};
    const struct sockaddr_in *sin = (const struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &sin->sin_addr, ipstr, sizeof(ipstr));
    ESP_LOGI(TAG, "DNS %s -> IPv4 %s:%u", host, ipstr, (unsigned)port);

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() 失败 errno=%d", errno);
        freeaddrinfo(res);
        return;
    }

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    int cr = connect(sock, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (cr != 0) {
        ESP_LOGE(TAG, "TCP connect %s:%u 失败 errno=%d（多半是 IPv6/路由/防火墙/被墙）",
                 ipstr, (unsigned)port, errno);
        close(sock);
        return;
    }
    ESP_LOGI(TAG, "TCP connect %s:%u OK（若 HTTPS 仍失败，问题在 TLS/证书/运营商）",
             ipstr, (unsigned)port);
    close(sock);
}

/**
 * @brief 安全追加 JSON 片段；缓冲不足或编码失败时截断并返回 false
 */
static bool json_append(char *buf, size_t cap, size_t *used, const char *fmt, ...)
{
    if (buf == NULL || used == NULL || cap == 0 || *used >= cap) {
        return false;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0) {
        buf[*used] = '\0';
        return false;
    }
    if ((size_t)n >= cap - *used) {
        /* C99 snprintf 返回“需要的长度”，不可直接累加 used */
        *used = cap - 1;
        return false;
    }
    *used += (size_t)n;
    return true;
}

/**
 * @brief 组装 readings JSON
 * @param backfill true：用 r->iso 作为历史 ts + measured_at（云端按 ts 落点）
 *                 false：实时上报，不发 ts（入库时间以服务端为准）
 */
static void build_json(const thp_reading_t *r, bool backfill, char *buf, size_t n)
{
    char live_iso[ISO_UTC_BUF_LEN];
    char device_part[96];
    char measured_part[64];
    char ts_part[64];
    char metrics[96];
    int rssi = r->rssi;
    size_t used = 0;
    bool metrics_ok = true;

    device_part[0] = '\0';
    {
        const char *did = device_id_for_kind(r->kind);
        if (did[0] != '\0') {
            snprintf(device_part, sizeof(device_part), ",\"device_id\":\"%s\"", did);
        }
    }

    measured_part[0] = '\0';
    ts_part[0] = '\0';
    metrics[0] = '\0';

    /* 部分字段：缺省传感器对应字段整体省略，不发 0/null 占位 */
    if (r->has_th) {
        metrics_ok = json_append(metrics, sizeof(metrics), &used,
                                 "\"temperature\":%.2f,\"humidity\":%.2f",
                                 (double)r->temperature, (double)r->humidity);
    }
    if (r->has_p) {
        if (metrics_ok && used > 0) {
            metrics_ok = json_append(metrics, sizeof(metrics), &used, ",");
        }
        if (metrics_ok) {
            metrics_ok = json_append(metrics, sizeof(metrics), &used,
                                     "\"pressure\":%.2f", (double)r->pressure);
        }
    }
    if (!metrics_ok && metrics[0] == '\0') {
        /* 无法组装业务字段：调用方应避免发送空 metrics（服务端会 400） */
        ESP_LOGE(TAG, "build_json metrics 缓冲不足，丢弃本帧");
    }

    if (backfill) {
        if (r->has_iso && r->iso[0] != '\0') {
            snprintf(measured_part, sizeof(measured_part), ",\"measured_at\":\"%s\"", r->iso);
            snprintf(ts_part, sizeof(ts_part), ",\"ts\":\"%s\"", r->iso);
        }
    } else if (format_iso_utc(live_iso)) {
        snprintf(measured_part, sizeof(measured_part), ",\"measured_at\":\"%s\"", live_iso);
    }

    snprintf(buf, n,
             "{%s%s%s%s,\"rssi\":%d}",
             metrics, device_part, measured_part, ts_part, rssi);
}

static thp_http_result_t classify_status(int status)
{
    if (status >= 200 && status < 300) {
        return THP_HTTP_OK;
    }
    if (status == 401 || status == 403) {
        return THP_HTTP_AUTH_FAIL;
    }
    if (status == 400) {
        return THP_HTTP_BAD_PAYLOAD;
    }
    return THP_HTTP_TRANSIENT;
}

/**
 * @brief POST 一次读数
 * @return 分类结果；*out_status 为 HTTP 状态码（网络错误时为负的 esp_http_client 错误）
 */
static thp_http_result_t report_once(const thp_reading_t *r, bool backfill, int *out_status)
{
    char url[256];
    char body[384];
    char auth[160];
    char resp[HTTP_RECV_BUF];

    size_t base_len = strlen(THP_API_BASE);
    const char *base = THP_API_BASE;
    /* 去掉末尾斜杠再拼接路径，避免 //api */
    while (base_len > 1 && base[base_len - 1] == '/') {
        base_len--;
    }
    if (base_len >= sizeof(url) - 24) {
        ESP_LOGE(TAG, "THP_API_BASE 过长");
        *out_status = -1;
        return THP_HTTP_TRANSIENT;
    }
    snprintf(url, sizeof(url), "%.*s/api/v1/readings", (int)base_len, base);

    build_json(r, backfill, body, sizeof(body));
    const char *token = token_for_kind(r->kind);
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = THP_HTTP_TIMEOUT_MS,
        .user_agent = "esp32c3-thp-report/1.0",
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
#if !THP_HTTP_SKIP_VERIFY
        /* Cloudflare/GTS：捆绑包匹配失败时，用内嵌 GTS Root R4 作为信任根
         * （同时保留 bundle，便于其它公网 CA；IDF 会同时解析两者） */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .cert_pem = THP_TLS_ROOT_PEM,
#endif
    };

#if THP_HTTP_SKIP_VERIFY
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = NULL;
    cfg.cert_pem = NULL;
#endif

    ESP_LOGI(TAG, "HTTP open heap=%u url=%s timeout=%dms backfill=%d kind=%s",
             (unsigned)esp_get_free_heap_size(), url, THP_HTTP_TIMEOUT_MS,
             (int)backfill, kind_tag(r->kind));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP 客户端初始化失败 heap=%u", (unsigned)esp_get_free_heap_size());
        *out_status = -1;
        return THP_HTTP_TRANSIENT;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 请求失败: %s (0x%x)  heap=%u  body=%s",
                 esp_err_to_name(err), (unsigned)err,
                 (unsigned)esp_get_free_heap_size(), body);
        /* 实时上报失败时做 TCP 预检；补传路径避免每条都刷预检日志 */
        if (!backfill) {
            probe_api_endpoint();
        }
        esp_http_client_cleanup(client);
        *out_status = -1;
        return THP_HTTP_TRANSIENT;
    }

    int status = esp_http_client_get_status_code(client);
    int content_len = esp_http_client_get_content_length(client);
    int read_len = esp_http_client_read(client, resp, sizeof(resp) - 1);
    if (read_len > 0) {
        resp[read_len] = '\0';
    } else {
        resp[0] = '\0';
    }
    esp_http_client_cleanup(client);

    *out_status = status;
    ESP_LOGI(TAG, "上报[%s] HTTP %d  body=%s  resp=%s",
             kind_tag(r->kind), status, body, resp[0] ? resp : "(empty)");

    if (content_len > HTTP_RECV_BUF - 1) {
        ESP_LOGW(TAG, "响应体超长已截断 (Content-Length=%d)", content_len);
    }

    return classify_status(status);
}

/**
 * @brief 有限次重试；backfill 时通常只试 1 次，失败留给下一轮再补
 * @return 最终分类结果（含 Wi-Fi 未连 / 重试耗尽 → TRANSIENT）
 */
static thp_http_result_t report_with_retry(const thp_reading_t *r, bool backfill, int max_retries)
{
    if (max_retries < 0) {
        max_retries = 0;
    }
    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "Wi-Fi 未连接，等待下一轮");
            return THP_HTTP_TRANSIENT;
        }

        int status = 0;
        thp_http_result_t res = report_once(r, backfill, &status);
        if (res == THP_HTTP_OK) {
            return THP_HTTP_OK;
        }
        if (res == THP_HTTP_AUTH_FAIL) {
            ESP_LOGE(TAG, "上报[%s] Token 无效或已吊销 (HTTP %d)，请在 Dash 重新生成",
                     kind_tag(r->kind), status);
            return THP_HTTP_AUTH_FAIL;
        }
        if (res == THP_HTTP_BAD_PAYLOAD) {
            ESP_LOGE(TAG, "服务端拒绝载荷[%s] (HTTP %d)，本周期不重试",
                     kind_tag(r->kind), status);
            return THP_HTTP_BAD_PAYLOAD;
        }

        if (attempt < max_retries) {
            uint32_t backoff = THP_REPORT_RETRY_BASE_MS * (1u << attempt);
            ESP_LOGW(TAG, "上报暂态失败 (attempt=%d/%d status=%d)，%lums 后重试",
                     attempt + 1, max_retries, status,
                     (unsigned long)backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
        } else {
            ESP_LOGW(TAG, "上报暂态失败 (attempt=%d/%d status=%d)%s",
                     attempt + 1, max_retries + 1, status,
                     backfill ? "，本条暂留队列" : "，将入离线队列");
        }
    }
    return THP_HTTP_TRANSIENT;
}

/**
 * @brief 网络恢复后按时间顺序补传队列（body 带历史 ts）
 *        每周期最多补 THP_OFFLINE_FLUSH_MAX_PER_CYCLE 条，避免长时间占住实时采样
 */
static void offline_queue_flush(void)
{
    if (s_offline_count == 0 || !wifi_is_connected()) {
        return;
    }

    ESP_LOGI(TAG, "开始补传离线队列，共 %u 条（本周期最多 %d）",
             (unsigned)s_offline_count, THP_OFFLINE_FLUSH_MAX_PER_CYCLE);

    int sent_this_cycle = 0;
    while (s_offline_count > 0) {
        if (sent_this_cycle >= THP_OFFLINE_FLUSH_MAX_PER_CYCLE) {
            ESP_LOGW(TAG, "本周期补传已达上限 %d，剩余 %u 条下轮继续",
                     THP_OFFLINE_FLUSH_MAX_PER_CYCLE, (unsigned)s_offline_count);
            return;
        }
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "补传中断：Wi-Fi 断开，剩余 %u 条", (unsigned)s_offline_count);
            return;
        }
        const thp_reading_t *item = offline_queue_peek();
        if (item == NULL) {
            break;
        }

        thp_http_result_t res = report_with_retry(item, true, 0);
        if (res == THP_HTTP_OK) {
            ESP_LOGI(TAG, "补传成功 kind=%s iso=%s",
                     kind_tag(item->kind), item->has_iso ? item->iso : "(no-ts)");
            offline_queue_pop();
            sent_this_cycle++;
            if (s_offline_count > 0 && sent_this_cycle < THP_OFFLINE_FLUSH_MAX_PER_CYCLE) {
                vTaskDelay(pdMS_TO_TICKS(THP_OFFLINE_FLUSH_GAP_MS));
            }
            continue;
        }
        if (res == THP_HTTP_AUTH_FAIL) {
            ESP_LOGE(TAG, "补传遇 Token 失效 kind=%s，清空离线队列 %u 条（重烧配置前无意义）",
                     kind_tag(item->kind), (unsigned)s_offline_count);
            offline_queue_clear();
            return;
        }
        if (res == THP_HTTP_BAD_PAYLOAD) {
            ESP_LOGW(TAG, "补传条被服务端拒绝 kind=%s，丢弃 iso=%s",
                     kind_tag(item->kind),
                     item->has_iso ? item->iso : "(no-ts)");
            offline_queue_pop();
            continue;
        }
        /* 暂态失败：网络仍不稳，保留剩余，下周期再试 */
        ESP_LOGW(TAG, "补传暂态失败，剩余 %u 条等待下一轮", (unsigned)s_offline_count);
        return;
    }

    ESP_LOGI(TAG, "离线队列已清空");
}

/* ---------------- 主流程 ---------------- */

#if THP_MI_ENABLE
/** 短窗扫描直到拿到未过期 MI 样本，或超时 */
static bool mi_fetch_sample(atc_ble_sample_t *out)
{
    if (!s_mi_ready) {
        return false;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    if (atc_ble_pop_latest(out) &&
        (now_ms - out->ts_ms) <= THP_MI_MAX_AGE_MS &&
        th_in_range(out->temperature, out->humidity)) {
        return true;
    }

    ESP_LOGI(TAG, "ATC 扫描开始 window=%dms", THP_MI_SCAN_BEFORE_REPORT_MS);
    (void)atc_ble_start_scan();
    const int64_t deadline = now_ms + THP_MI_SCAN_BEFORE_REPORT_MS;
    while ((esp_timer_get_time() / 1000) < deadline) {
        if (atc_ble_pop_latest(out) &&
            th_in_range(out->temperature, out->humidity)) {
            (void)atc_ble_stop_scan();
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    (void)atc_ble_stop_scan();

    if (atc_ble_pop_latest(out)) {
        const int64_t age = (esp_timer_get_time() / 1000) - out->ts_ms;
        ESP_LOGW(TAG, "ATC 扫描超时/样本过期 age=%lldms t=%.2f h=%.2f",
                 (long long)age, (double)out->temperature, (double)out->humidity);
    } else {
        ESP_LOGW(TAG, "ATC 扫描超时，无匹配 MAC/广播");
    }
    return false;
}

static void report_mi_if_ready(void)
{
    if (!s_mi_ready) {
        return;
    }
    unsigned heap = (unsigned)esp_get_free_heap_size();
    if (heap < 80000) {
        ESP_LOGW(TAG, "heap_free=%u < 80KB，本周期跳过 MI 上报", heap);
        return;
    }

    atc_ble_sample_t mi;
    if (!mi_fetch_sample(&mi)) {
        return;
    }

    thp_reading_t reading;
    reading_stamp_now(&reading);
    reading.kind = THP_KIND_MI;
    reading.temperature = mi.temperature;
    reading.humidity = mi.humidity;
    reading.pressure = 0;
    reading.has_th = true;
    reading.has_p = false;
    reading.rssi = (int)mi.rssi;

    ESP_LOGI(TAG, "MI 采样 T=%.2f°C H=%.2f%% rssi=%d batt=%u iso=%s",
             (double)mi.temperature, (double)mi.humidity,
             (int)mi.rssi,
             mi.battery_pct == 0xFF ? 0 : mi.battery_pct,
             reading.has_iso ? reading.iso : "(no-ts)");

    thp_http_result_t res = report_with_retry(&reading, false, THP_REPORT_MAX_RETRIES);
    if (res == THP_HTTP_TRANSIENT) {
        offline_queue_push(&reading);
    }
}
#endif /* THP_MI_ENABLE */

static void thp_report_task(void *arg)
{
    (void)arg;
    thp_sample_t sample;
    thp_reading_t reading;

    vTaskDelay(pdMS_TO_TICKS(THP_REPORT_FIRST_DELAY_MS));

    while (1) {
        /* 每次提交前用多 NTP 服务器同步系统时间（measured_at / 入队时间戳） */
        (void)sntp_sync_before_report();

        /* 有积压且网络可用时，先补历史再报当前，保证时间轴大致有序 */
        offline_queue_flush();

        if (sample_read(&sample)) {
            /* 先打 UTC/rssi 戳（会 memset），再写入业务字段 */
            reading_stamp_now(&reading);
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

            thp_http_result_t res = report_with_retry(&reading, false, THP_REPORT_MAX_RETRIES);
            if (res == THP_HTTP_TRANSIENT) {
                offline_queue_push(&reading);
            }
        } else {
            ESP_LOGW(TAG, "本周期无有效本机采样，跳过 LOCAL 上报");
        }

#if THP_MI_ENABLE
        /* HTTP/TLS 期间停扫描；MI 在本机上报之后处理 */
        report_mi_if_ready();
#endif

        vTaskDelay(pdMS_TO_TICKS(THP_REPORT_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "THP Dash 设备端启动  period=%dms  offline_q=%d  base=%s",
             THP_REPORT_PERIOD_MS, THP_OFFLINE_QUEUE_LEN, THP_API_BASE);

    if (strcmp(THP_DEVICE_TOKEN, "thp_replace_me") == 0 ||
        strlen(THP_DEVICE_TOKEN) < 8) {
        ESP_LOGE(TAG, "请先配置 thp_config.h 中的 THP_DEVICE_TOKEN（Dash 生成，明文只显示一次）");
    }

#if THP_MI_ENABLE
    if (!mi_token_ok()) {
        ESP_LOGW(TAG, "MI Token 未配置（THP_MI_DEVICE_TOKEN），跳过小米计上报");
    } else {
        uint8_t mac[6];
        uint8_t key[16];
        bool mac_ok = atc_ble_parse_mac_str(THP_MI_MAC, mac);
        if (!mac_ok) {
            ESP_LOGW(TAG, "THP_MI_MAC 非法: '%s'（示例 A4:C1:38:E2:4E:43）", THP_MI_MAC);
        }
        bool key_ok = atc_ble_parse_key_hex(THP_MI_BINDKEY, key);
        if (!key_ok) {
            memset(key, 0, sizeof(key));
            ESP_LOGW(TAG, "THP_MI_BINDKEY 非法或未填；明文 Custom 可工作，加密 beacon 需要 BindKey");
        }
        if (mac_ok) {
            esp_err_t mi_err = atc_ble_init(mac, key_ok ? key : NULL);
            if (mi_err == ESP_OK) {
                s_mi_ready = true;
                ESP_LOGI(TAG, "MI BLE 网关就绪 mac=%s enc_key=%d", THP_MI_MAC, (int)key_ok);
            } else {
                ESP_LOGE(TAG, "atc_ble_init 失败: %s", esp_err_to_name(mi_err));
            }
        }
    }
#else
    ESP_LOGI(TAG, "THP_MI_ENABLE=0，未编译小米计 BLE 网关");
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    i2c_lines_selftest();
    ESP_ERROR_CHECK(i2c_bus_init());

    err = sht40_init(&s_sht, s_bus, I2C_SCL_SPEED_HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT40 初始化失败：暂仅上报气压（若有）；任务内会重试探测（禁止用 BMP 温度替代）");
    }
    err = bmp280_init(&s_bmp, s_bus, I2C_SCL_SPEED_HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMP280 初始化失败：暂仅上报温湿度（若有）；任务内会重试探测");
    }

    if (wifi_init_sta() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 未就绪，设备将在重试/复位策略下继续尝试连接");
        /* 仍启动上报任务：任务内会等待 Wi-Fi 位；同时后台继续有限重连由事件驱动 */
        /* 简单策略：循环重新 connect */
        for (;;) {
            esp_wifi_connect();
            EventBits_t bits = xEventGroupWaitBits(
                s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
                pdMS_TO_TICKS(10000));
            if (bits & WIFI_CONNECTED_BIT) {
                break;
            }
        }
    }

    sntp_start();

    probe_api_endpoint();

    if (!s_sht.present || !s_bmp.present) {
        ESP_LOGW(TAG, "传感器未全部就绪（SHT40=%d BMP280=%d），支持仅温湿度或仅气压上报；任务内热修复重试",
                 s_sht.present, s_bmp.present);
    }

    xTaskCreate(thp_report_task, "thp_report", 8192, NULL, 5, NULL);
}
