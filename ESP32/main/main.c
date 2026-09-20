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

static const char *TAG = "thp";

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
 * @brief 生成 ISO-8601 UTC 时间戳，如 2026-01-01T12:00:00.000Z
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
    int ms = (int)(tv.tv_usec / 1000);
    if (ms < 0) {
        ms = 0;
    } else if (ms > 999) {
        ms = 999;
    }

    snprintf(out, ISO_UTC_BUF_LEN, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             year, mon, day, hour, min, sec, ms);
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
    bool  valid;
} thp_sample_t;

/** 与 Worker validateReadingPayload 对齐（src/routes/readings.js） */
static bool sample_in_range(const thp_sample_t *s)
{
    if (s->temperature < -40.0f || s->temperature > 85.0f) {
        return false;
    }
    if (s->humidity < 0.0f || s->humidity > 100.0f) {
        return false;
    }
    if (s->pressure < 300.0f || s->pressure > 1200.0f) {
        return false;
    }
    return true;
}

static bool sample_read(thp_sample_t *out)
{
    memset(out, 0, sizeof(*out));

    float sht_t = 0.0f, sht_h = 0.0f;
    float bmp_t = 0.0f, bmp_p = 0.0f, bmp_alt = 0.0f;
    int32_t t_fine = 0;

    bool sht_ok = (sht40_read(&s_sht, &sht_t, &sht_h) == ESP_OK);
    /* bmp_t 仅用于内部补偿展示；业务温度不使用 BMP280 */
    bool bmp_ok = (bmp280_read(&s_bmp, &bmp_t, &bmp_p, &bmp_alt, &t_fine) == ESP_OK);

    if (!sht_ok || !bmp_ok) {
        (void)i2c_master_bus_reset(s_bus);
    }

    /* 口径：T/H = SHT40，P = BMP280。任一权威来源失败则本帧不上报。 */
    if (!sht_ok || !bmp_ok) {
        ESP_LOGW(TAG, "采样失败: SHT40=%s BMP280=%s", sht_ok ? "OK" : "FAIL", bmp_ok ? "OK" : "FAIL");
        return false;
    }

    out->temperature = sht_t;
    out->humidity = sht_h;
    out->pressure = bmp_p;
    out->valid = sample_in_range(out);
    if (!out->valid) {
        ESP_LOGW(TAG, "采样超范围，跳过上报: T=%.2f H=%.2f P=%.2f (BMP 内部 T=%.2f 仅参考)",
                 (double)out->temperature, (double)out->humidity,
                 (double)out->pressure, (double)bmp_t);
        return false;
    }
    return true;
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

static void build_json(const thp_sample_t *s, char *buf, size_t n)
{
    char iso[ISO_UTC_BUF_LEN];
    char device_part[96];
    char measured_part[64];
    int rssi = 0;
    wifi_ap_record_t ap;

    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        rssi = ap.rssi;
    }

    device_part[0] = '\0';
    if (THP_DEVICE_ID[0] != '\0') {
        snprintf(device_part, sizeof(device_part), ",\"device_id\":\"%s\"", THP_DEVICE_ID);
    }

    measured_part[0] = '\0';
    if (format_iso_utc(iso)) {
        snprintf(measured_part, sizeof(measured_part), ",\"measured_at\":\"%s\"", iso);
    }

    snprintf(buf, n,
             "{\"temperature\":%.2f,\"humidity\":%.2f,\"pressure\":%.2f%s%s,\"rssi\":%d}",
             (double)s->temperature, (double)s->humidity, (double)s->pressure,
             device_part, measured_part, rssi);
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
static thp_http_result_t report_once(const thp_sample_t *s, int *out_status)
{
    char url[256];
    char body[320];
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

    build_json(s, body, sizeof(body));
    snprintf(auth, sizeof(auth), "Bearer %s", THP_DEVICE_TOKEN);

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

    ESP_LOGI(TAG, "HTTP open heap=%u url=%s timeout=%dms",
             (unsigned)esp_get_free_heap_size(), url, THP_HTTP_TIMEOUT_MS);

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
        /* 紧接着做一次 TCP 预检，方便区分 DNS / 路由 / TLS */
        probe_api_endpoint();
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
    ESP_LOGI(TAG, "上报 HTTP %d  body=%s  resp=%s",
             status, body, resp[0] ? resp : "(empty)");

    if (content_len > HTTP_RECV_BUF - 1) {
        ESP_LOGW(TAG, "响应体超长已截断 (Content-Length=%d)", content_len);
    }

    return classify_status(status);
}

/**
 * @brief 按需求 §10：401/403/400 不重发；网络/5xx 有限次数退避重试
 */
static void report_with_retry(const thp_sample_t *s)
{
    for (int attempt = 0; attempt <= THP_REPORT_MAX_RETRIES; attempt++) {
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "Wi-Fi 未连接，等待下一轮");
            return;
        }

        int status = 0;
        thp_http_result_t r = report_once(s, &status);
        if (r == THP_HTTP_OK) {
            return;
        }
        if (r == THP_HTTP_AUTH_FAIL) {
            ESP_LOGE(TAG, "Token 无效或已吊销 (HTTP %d)，请在 Dash 重新生成", status);
            return;
        }
        if (r == THP_HTTP_BAD_PAYLOAD) {
            ESP_LOGE(TAG, "服务端拒绝载荷 (HTTP %d)，本周期不重试", status);
            return;
        }

        if (attempt < THP_REPORT_MAX_RETRIES) {
            uint32_t backoff = THP_REPORT_RETRY_BASE_MS * (1u << attempt);
            ESP_LOGW(TAG, "上报暂态失败 (attempt=%d/%d status=%d)，%lums 后重试",
                     attempt + 1, THP_REPORT_MAX_RETRIES, status,
                     (unsigned long)backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
        } else {
            ESP_LOGE(TAG, "上报失败，已达最大重试次数，等待下一周期");
        }
    }
}

/* ---------------- 主流程 ---------------- */

static void thp_report_task(void *arg)
{
    (void)arg;
    thp_sample_t sample;

    vTaskDelay(pdMS_TO_TICKS(THP_REPORT_FIRST_DELAY_MS));

    while (1) {
        /* 每次提交前用多 NTP 服务器同步系统时间（measured_at） */
        (void)sntp_sync_before_report();

        if (sample_read(&sample)) {
            ESP_LOGI(TAG, "采样 T=%.2f°C H=%.2f%% P=%.2fhPa → %s",
                     (double)sample.temperature, (double)sample.humidity,
                     (double)sample.pressure, THP_API_BASE);
            report_with_retry(&sample);
        } else {
            ESP_LOGW(TAG, "本周期无有效采样，跳过上报");
        }

        vTaskDelay(pdMS_TO_TICKS(THP_REPORT_PERIOD_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "THP Dash 设备端启动  period=%dms  base=%s",
             THP_REPORT_PERIOD_MS, THP_API_BASE);

    if (strcmp(THP_DEVICE_TOKEN, "thp_replace_me") == 0 ||
        strlen(THP_DEVICE_TOKEN) < 8) {
        ESP_LOGE(TAG, "请先配置 thp_config.h 中的 THP_DEVICE_TOKEN（Dash 生成，明文只显示一次）");
    }

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
        ESP_LOGE(TAG, "SHT40 初始化失败：无温湿度则无法上报（口径禁止用 BMP 温度替代）");
    }
    err = bmp280_init(&s_bmp, s_bus, I2C_SCL_SPEED_HZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BMP280 初始化失败：无气压则无法上报");
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
        ESP_LOGE(TAG, "传感器未全部就绪（SHT40=%d BMP280=%d），上报任务仍启动以便热修复后重试读",
                 s_sht.present, s_bmp.present);
    }

    xTaskCreate(thp_report_task, "thp_report", 8192, NULL, 5, NULL);
}
