#include "thp_report.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include "atc_ble.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/ip_addr.h"

#include "thp_config.h"
#include "thp_queue.h"
#include "thp_sources.h"
#include "thp_time.h"
#include "thp_tls_trust.h"
#include "thp_types.h"
#include "thp_wifi.h"

static const char *TAG = "thp.report";

#ifndef THP_HTTP_TIMEOUT_MS
#define THP_HTTP_TIMEOUT_MS 15000
#endif
#ifndef THP_OFFLINE_FLUSH_GAP_MS
#define THP_OFFLINE_FLUSH_GAP_MS 200
#endif
#ifndef THP_OFFLINE_FLUSH_MAX_PER_CYCLE
#define THP_OFFLINE_FLUSH_MAX_PER_CYCLE 24
#endif
#ifndef THP_REPORT_MAX_RETRIES
#define THP_REPORT_MAX_RETRIES 3
#endif
#ifndef THP_REPORT_RETRY_BASE_MS
#define THP_REPORT_RETRY_BASE_MS 2000
#endif
#define THP_PROBE_MIN_REMAIN_MS 20000
#define HTTP_RECV_BUF 1024

static SemaphoreHandle_t s_net_mtx;
static bool s_probe_this_cycle;

void thp_report_new_cycle(void)
{
    s_probe_this_cycle = false;
}

esp_err_t thp_report_init(void)
{
    s_net_mtx = xSemaphoreCreateMutex();
    if (s_net_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

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

void thp_report_probe_api(void)
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
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS 解析失败 host=%s gai=%d", host, gai);
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
        ESP_LOGE(TAG, "TCP connect %s:%u 失败 errno=%d", ipstr, (unsigned)port, errno);
        close(sock);
        return;
    }
    ESP_LOGI(TAG, "TCP connect %s:%u OK", ipstr, (unsigned)port);
    close(sock);
}

static void maybe_probe_api(TickType_t deadline, bool backfill)
{
    if (backfill || s_probe_this_cycle) {
        return;
    }
    int32_t remain = thp_deadline_remain_ms(deadline);
    if (remain < THP_PROBE_MIN_REMAIN_MS) {
        s_probe_this_cycle = true;
        return;
    }
    s_probe_this_cycle = true;
    thp_report_probe_api();
}

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
        *used = cap - 1;
        return false;
    }
    *used += (size_t)n;
    return true;
}

/** 将 device_id 转义后写入 JSON 字符串字面量（不含引号） */
static bool json_escape_into(char *dst, size_t cap, const char *src)
{
    size_t o = 0;
    if (dst == NULL || cap == 0) {
        return false;
    }
    for (const char *p = src; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) {
                dst[0] = '\0';
                return false;
            }
            dst[o++] = '\\';
            dst[o++] = (char)c;
            continue;
        }
        if (c < 0x20) {
            if (o + 6 >= cap) {
                dst[0] = '\0';
                return false;
            }
            snprintf(dst + o, cap - o, "\\u%04x", (unsigned)c);
            o += 6;
            continue;
        }
        if (o + 1 >= cap) {
            dst[0] = '\0';
            return false;
        }
        dst[o++] = (char)c;
    }
    dst[o] = '\0';
    return true;
}

/** 成功写出完整 metrics 与可选附加字段；metrics 为空则失败（禁止 `{,\"rssi\"…}`） */
static bool build_json(const thp_reading_t *r, bool backfill, char *buf, size_t n)
{
    char live_iso[ISO_UTC_BUF_LEN];
    char device_part[128];
    char measured_part[64];
    char ts_part[64];
    char metrics[96];
    char escaped_id[96];
    int rssi = r->rssi;
    size_t used = 0;
    bool metrics_ok = true;
    bool any_metric = r->has_th || r->has_p;

    if (buf == NULL || n == 0 || !any_metric) {
        if (buf != NULL && n > 0) {
            buf[0] = '\0';
        }
        return false;
    }

    device_part[0] = '\0';
    {
        const char *did = thp_source_device_id(r->source_id);
        if (did != NULL && did[0] != '\0' &&
            json_escape_into(escaped_id, sizeof(escaped_id), did)) {
            snprintf(device_part, sizeof(device_part), ",\"device_id\":\"%s\"", escaped_id);
        }
    }

    measured_part[0] = '\0';
    ts_part[0] = '\0';
    metrics[0] = '\0';

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
    if (!metrics_ok) {
        ESP_LOGE(TAG, "build_json metrics 缓冲不足，丢弃本帧");
        buf[0] = '\0';
        return false;
    }

    if (backfill) {
        if (r->has_iso && r->iso[0] != '\0') {
            snprintf(measured_part, sizeof(measured_part), ",\"measured_at\":\"%s\"", r->iso);
            snprintf(ts_part, sizeof(ts_part), ",\"ts\":\"%s\"", r->iso);
        }
    } else if (thp_time_format_iso(live_iso)) {
        snprintf(measured_part, sizeof(measured_part), ",\"measured_at\":\"%s\"", live_iso);
    }

    int wr = snprintf(buf, n,
                      "{%s%s%s%s,\"rssi\":%d}",
                      metrics, device_part, measured_part, ts_part, rssi);
    if (wr <= 0 || (size_t)wr >= n) {
        ESP_LOGE(TAG, "build_json 体超长已丢弃");
        buf[0] = '\0';
        return false;
    }
    return true;
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

static thp_http_result_t report_once(const thp_reading_t *r, bool backfill,
                                     TickType_t deadline, int *out_status)
{
    char url[256];
    char body[384];
    char auth[160];
    char resp[HTTP_RECV_BUF];

    if (thp_deadline_reached(deadline)) {
        *out_status = -1;
        return THP_HTTP_TRANSIENT;
    }

    size_t base_len = strlen(THP_API_BASE);
    const char *base = THP_API_BASE;
    while (base_len > 1 && base[base_len - 1] == '/') {
        base_len--;
    }
    if (base_len >= sizeof(url) - 24) {
        ESP_LOGE(TAG, "THP_API_BASE 过长");
        *out_status = -1;
        return THP_HTTP_TRANSIENT;
    }
    snprintf(url, sizeof(url), "%.*s/api/v1/readings", (int)base_len, base);

    if (!build_json(r, backfill, body, sizeof(body))) {
        ESP_LOGE(TAG, "build_json 失败 src=%s，丢弃本帧", thp_source_name(r->source_id));
        *out_status = -1;
        return THP_HTTP_BAD_PAYLOAD;
    }
    const char *token = thp_source_token(r->source_id);
    snprintf(auth, sizeof(auth), "Bearer %s", token);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = THP_HTTP_TIMEOUT_MS,
        .user_agent = "esp32c3-thp-multi/1.0",
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .disable_auto_redirect = true,
#if !THP_HTTP_SKIP_VERIFY
        .crt_bundle_attach = esp_crt_bundle_attach,
        .cert_pem = THP_TLS_ROOT_PEM,
#endif
    };

#if THP_HTTP_SKIP_VERIFY
    cfg.skip_cert_common_name_check = true;
    cfg.crt_bundle_attach = NULL;
    cfg.cert_pem = NULL;
#endif

    ESP_LOGI(TAG, "HTTP open heap=%u src=%s backfill=%d remain=%dms",
             (unsigned)esp_get_free_heap_size(), thp_source_name(r->source_id),
             (int)backfill, (int)thp_deadline_remain_ms(deadline));

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        *out_status = -1;
        return THP_HTTP_TRANSIENT;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_header(client, "Connection", "close");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    bool net_locked = false;
    if (s_net_mtx) {
        net_locked = xSemaphoreTake(s_net_mtx, pdMS_TO_TICKS(THP_HTTP_TIMEOUT_MS)) == pdTRUE;
        if (!net_locked) {
            esp_http_client_cleanup(client);
            *out_status = -1;
            return THP_HTTP_TRANSIENT;
        }
    }

    atc_ble_stop_scan();
    esp_err_t err = esp_http_client_perform(client);
    atc_ble_resume_scan_if_wanted();

    if (net_locked && s_net_mtx) {
        xSemaphoreGive(s_net_mtx);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 请求失败: %s (0x%x) heap=%u body=%s",
                 esp_err_to_name(err), (unsigned)err,
                 (unsigned)esp_get_free_heap_size(), body);
        maybe_probe_api(deadline, backfill);
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

    if (content_len > HTTP_RECV_BUF - 1) {
        ESP_LOGW(TAG, "响应体超长已截断 (Content-Length=%d)", content_len);
    }

    *out_status = status;
    ESP_LOGI(TAG, "上报[%s] HTTP %d  body=%s  resp=%s",
             thp_source_name(r->source_id), status, body, resp[0] ? resp : "(empty)");
    return classify_status(status);
}

thp_http_result_t thp_report_with_retry(const thp_reading_t *r, bool backfill,
                                        int max_retries, TickType_t deadline)
{
    if (max_retries < 0) {
        max_retries = 0;
    }
    for (int attempt = 0; attempt <= max_retries; attempt++) {
        if (!thp_wifi_is_connected()) {
            return THP_HTTP_TRANSIENT;
        }
        if (thp_deadline_reached(deadline)) {
            ESP_LOGW(TAG, "周期 deadline 已到，停止上报 src=%s", thp_source_name(r->source_id));
            return THP_HTTP_TRANSIENT;
        }

        int status = 0;
        thp_http_result_t res = report_once(r, backfill, deadline, &status);
        if (res == THP_HTTP_OK) {
            return THP_HTTP_OK;
        }
        if (res == THP_HTTP_AUTH_FAIL) {
            ESP_LOGE(TAG, "Token 无效或已吊销 src=%s (HTTP %d)",
                     thp_source_name(r->source_id), status);
            return THP_HTTP_AUTH_FAIL;
        }
        if (res == THP_HTTP_BAD_PAYLOAD) {
            ESP_LOGE(TAG, "服务端拒绝载荷 src=%s (HTTP %d)",
                     thp_source_name(r->source_id), status);
            return THP_HTTP_BAD_PAYLOAD;
        }

        if (attempt < max_retries) {
            uint32_t backoff = THP_REPORT_RETRY_BASE_MS * (1u << attempt);
            if (thp_deadline_remain_ms(deadline) < (int32_t)backoff + 500) {
                return THP_HTTP_TRANSIENT;
            }
            ESP_LOGW(TAG, "上报暂态失败 src=%s attempt=%d，%lums 后重试",
                     thp_source_name(r->source_id), attempt + 1,
                     (unsigned long)backoff);
            vTaskDelay(pdMS_TO_TICKS(backoff));
        }
    }
    return THP_HTTP_TRANSIENT;
}

void thp_report_or_enqueue(const thp_reading_t *r, TickType_t deadline)
{
    if (r == NULL) {
        return;
    }
    if (!thp_wifi_is_connected() || thp_deadline_reached(deadline)) {
        thp_queue_push(r);
        return;
    }

    thp_http_result_t res = thp_report_with_retry(r, false, THP_REPORT_MAX_RETRIES, deadline);
    if (res == THP_HTTP_TRANSIENT) {
        thp_queue_push(r);
    } else if (res == THP_HTTP_AUTH_FAIL) {
        ESP_LOGE(TAG, "Token 失效 src=%s，清除积压并停用该源（需更新 Token 后重编译）",
                 thp_source_name(r->source_id));
        thp_queue_clear_source(r->source_id);
        thp_source_set_ready(r->source_id, false);
    }
}

void thp_report_flush_queue(int max_items, TickType_t deadline)
{
    if (max_items <= 0) {
        max_items = THP_OFFLINE_FLUSH_MAX_PER_CYCLE;
    }
    if (!thp_wifi_is_connected() || thp_deadline_reached(deadline)) {
        return;
    }
    unsigned pending = thp_queue_count();
    if (pending == 0) {
        return;
    }

    ESP_LOGI(TAG, "开始补传离线队列，共 %u 条（本周期最多 %d）", pending, max_items);

    int sent_this_cycle = 0;
    while (sent_this_cycle < max_items) {
        if (thp_deadline_reached(deadline) || !thp_wifi_is_connected()) {
            return;
        }
        thp_reading_t item;
        if (!thp_queue_peek_copy(&item)) {
            break;
        }

        thp_http_result_t res = thp_report_with_retry(&item, true, 0, deadline);
        if (res == THP_HTTP_OK) {
            ESP_LOGI(TAG, "补传成功 src=%s iso=%s",
                     thp_source_name(item.source_id),
                     item.has_iso ? item.iso : "(no-ts)");
            thp_queue_pop();
            sent_this_cycle++;
            if (sent_this_cycle < max_items && !thp_deadline_reached(deadline)) {
                vTaskDelay(pdMS_TO_TICKS(THP_OFFLINE_FLUSH_GAP_MS));
            }
            continue;
        }
        if (res == THP_HTTP_AUTH_FAIL) {
            ESP_LOGE(TAG, "补传遇 Token 失效 src=%s，清该源积压并停用",
                     thp_source_name(item.source_id));
            thp_queue_clear_source(item.source_id);
            thp_source_set_ready(item.source_id, false);
            continue;
        }
        if (res == THP_HTTP_BAD_PAYLOAD) {
            thp_queue_pop();
            continue;
        }
        ESP_LOGW(TAG, "补传暂态失败，剩余等待下一周期");
        return;
    }

    unsigned left = thp_queue_count();
    if (left == 0) {
        ESP_LOGI(TAG, "离线队列已清空");
    } else {
        ESP_LOGI(TAG, "离线队列剩余 %u 条", left);
    }
}
