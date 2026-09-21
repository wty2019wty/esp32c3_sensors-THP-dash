/*
 * atc_ble — pvvx ATC_MiThermometer 被动扫描
 * 支持：
 *   1) PVVX (Custom) 明文  — Service Data UUID 0x181A，size=18
 *   2) PVVX (Custom) 加密  — 同 UUID，size=14，AES-CCM + BindKey（AtcMiCodec）
 *
 * AtcMiCodec（与 pvvx python-interface 一致）：
 *   header = AD 前 4 字节 [size][0x16][0x1A][0x18]
 *   nonce  = adv_mac.reverse() + header + codec[0]
 *   AAD    = 0x11
 *   cipher = codec[1 .. -4]，MIC = codec[-4 ..]
 *   plain  = int16 t*0.01 | uint16 h*0.01 | uint8 batt% | uint8 flags
 */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* IDF 6.x / mbedtls 4：CCM 走 PSA，不再暴露 mbedtls/ccm.h */
#include "psa/crypto.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_store.h"
#include "host/util/util.h"

#include "atc_ble.h"

static const char *TAG = "atc_ble";

#define UUID16_ENV_SENSE   0x181A
#define AD_TYPE_SERVICE16  0x16

static atc_ble_sample_t s_latest;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_expect_mac[6];
static uint8_t s_bindkey[16];
static bool s_has_mac_filter;
static bool s_has_bindkey;
/* 跨任务读写（GAP 回调 / scan_sup / thp_cycle 上报路径） */
static volatile bool s_scanning;
static volatile bool s_inited;
static volatile bool s_synced;
static volatile bool s_scan_wanted = false; /* 仅窗口打开时 true；HTTP perform 期间短暂 false */
static bool s_scan_announced;
static uint8_t s_own_addr_type;

/* 窗口环形缓存：收集窗口内帧，上报时取 |ts-ref| 最小 */
#define ATC_WINDOW_RING_N 24
static atc_ble_sample_t s_win_ring[ATC_WINDOW_RING_N];
static volatile size_t s_win_count;
static volatile size_t s_win_head;
static volatile bool s_window_active;
static volatile int64_t s_window_open_ms;
static volatile int64_t s_window_close_ms;

static int gap_on_event(struct ble_gap_event *event, void *arg);
static void start_scan_locked(void);
static void scan_sup_task(void *arg);

/* ---------------- helpers ---------------- */

bool atc_ble_parse_mac_str(const char *s, uint8_t out[6])
{
    if (s == NULL || out == NULL) {
        return false;
    }
    char hex[13];
    int h = 0;
    for (const char *p = s; *p && h < 12; p++) {
        if (*p == ':' || *p == '-' || *p == ' ' || *p == '.') {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        hex[h++] = (char)tolower((unsigned char)*p);
    }
    if (h != 12) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    return true;
}

bool atc_ble_parse_key_hex(const char *s, uint8_t out[16])
{
    if (s == NULL || out == NULL) {
        return false;
    }
    char hex[33];
    int h = 0;
    for (const char *p = s; *p && h < 32; p++) {
        if (*p == ':' || *p == '-' || *p == ' ') {
            continue;
        }
        if (!isxdigit((unsigned char)*p)) {
            return false;
        }
        hex[h++] = (char)tolower((unsigned char)*p);
    }
    if (h != 32) {
        return false;
    }
    for (int i = 0; i < 16; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        out[i] = (uint8_t)strtoul(b, NULL, 16);
    }
    return true;
}

static bool th_range_ok(float t, float h)
{
    return t >= -40.0f && t <= 85.0f && h >= 0.0f && h <= 100.0f;
}

static bool mac_eq(const uint8_t a[6], const uint8_t b[6])
{
    return memcmp(a, b, 6) == 0;
}

static void cache_store(const atc_ble_sample_t *s)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    s_latest = *s;
    s_latest.valid = true;
    s_latest.ts_ms = now_ms;
    if (s_window_active &&
        now_ms >= s_window_open_ms && now_ms <= s_window_close_ms) {
        s_win_ring[s_win_head] = s_latest;
        s_win_head = (s_win_head + 1) % ATC_WINDOW_RING_N;
        if (s_win_count < ATC_WINDOW_RING_N) {
            s_win_count++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ---------------- parsers ---------------- */

bool atc_ble_parse_pvvx_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                              const uint8_t adv_mac[6], atc_ble_sample_t *out)
{
    if (after_uuid == NULL || out == NULL || after_uuid_len < 15) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->battery_pct = 0xFF;

    /* MAC 在包内为 LSB first（ReversedMacAddress） */
    for (int i = 0; i < 6; i++) {
        out->mac[i] = after_uuid[5 - i];
    }
    if (adv_mac != NULL && s_has_mac_filter && !mac_eq(out->mac, s_expect_mac)) {
        /* 包内 MAC 与配置不符时仍可看广播地址 */
        if (!mac_eq(adv_mac, s_expect_mac) && !mac_eq(out->mac, s_expect_mac)) {
            return false;
        }
    }

    int16_t t100 = (int16_t)(after_uuid[6] | (after_uuid[7] << 8));
    uint16_t h100 = (uint16_t)(after_uuid[8] | (after_uuid[9] << 8));
    out->temperature = t100 / 100.0f;
    out->humidity = h100 / 100.0f;
    out->battery_mv = (uint16_t)(after_uuid[10] | (after_uuid[11] << 8));
    out->battery_pct = after_uuid[12];
    out->adv_counter = after_uuid[13];
    out->flags = after_uuid[14];

    if (!th_range_ok(out->temperature, out->humidity)) {
        return false;
    }
    return true;
}

bool atc_ble_parse_pvvx_encrypted(const uint8_t *ad, uint16_t ad_len,
                                  const uint8_t adv_mac[6],
                                  const uint8_t bindkey[16],
                                  atc_ble_sample_t *out)
{
    if (ad == NULL || out == NULL || adv_mac == NULL || bindkey == NULL) {
        return false;
    }
    /* size + 0x16 + uuid2 + codec0 + cipher(6) + mic(4) */
    if (ad_len < 4 + 1 + 6 + 4) {
        return false;
    }
    if (ad[1] != AD_TYPE_SERVICE16 || ad[2] != 0x1A || ad[3] != 0x18) {
        return false;
    }
    /* 明文 Custom size=18(0x12)，加密 size=14(0x0e)；也接受其它 size 只要结构匹配 */
    const uint8_t *codec = ad + 4;
    uint16_t codec_len = (uint16_t)(ad_len - 4);
    if (codec_len < 1 + 6 + 4) {
        return false;
    }

    uint8_t nonce[11];
    /* adv_mac 显示序 MSB first → reverse 与 AtcMiCodec mac[::-1] 一致 */
    for (int i = 0; i < 6; i++) {
        nonce[i] = adv_mac[5 - i];
    }
    memcpy(nonce + 6, ad, 4);
    nonce[10] = codec[0];

    const uint8_t *cipher = codec + 1;
    size_t cipher_len = codec_len - 1 - 4;
    const uint8_t *mic = codec + codec_len - 4;
    const uint8_t aad = 0x11;
    uint8_t plain[16];
    if (cipher_len > sizeof(plain) || cipher_len < 6) {
        return false;
    }

    /* 组装 CCM 密文||MIC，PSA AEAD 一次性校验解密（tag 长度 4） */
    uint8_t ct_and_tag[20];
    if (cipher_len + 4 > sizeof(ct_and_tag)) {
        return false;
    }
    memcpy(ct_and_tag, cipher, cipher_len);
    memcpy(ct_and_tag + cipher_len, mic, 4);

    psa_status_t st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_crypto_init: %d", (int)st);
        return false;
    }

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DECRYPT | PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4));

    psa_key_id_t key_id = PSA_KEY_ID_NULL;
    st = psa_import_key(&attr, bindkey, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_import_key: %d", (int)st);
        return false;
    }

    size_t plain_len = 0;
    st = psa_aead_decrypt(key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4),
                          nonce, sizeof(nonce),
                          &aad, 1,
                          ct_and_tag, cipher_len + 4,
                          plain, sizeof(plain), &plain_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGD(TAG, "psa_aead_decrypt: %d (bindkey/MAC/密文不符)", (int)st);
        return false;
    }
    if (plain_len < 6) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->battery_mv = 0;
    out->battery_pct = 0xFF;
    memcpy(out->mac, adv_mac, 6);
    int16_t t100 = (int16_t)(plain[0] | (plain[1] << 8));
    uint16_t h100 = (uint16_t)(plain[2] | (plain[3] << 8));
    out->temperature = t100 / 100.0f;
    out->humidity = h100 / 100.0f;
    out->battery_pct = plain[4];
    out->flags = plain[5];
    if (!th_range_ok(out->temperature, out->humidity)) {
        return false;
    }
    return true;
}

/** 在原始广播数据中找 Service Data 0x181A 并解析 */
static void handle_adv_raw(const uint8_t addr_msb[6], int8_t rssi,
                           const uint8_t *data, uint8_t len)
{
    if (data == NULL || len < 3) {
        return;
    }
    if (s_has_mac_filter && !mac_eq(addr_msb, s_expect_mac)) {
        return;
    }

    size_t off = 0;
    while (off + 1 < len) {
        uint8_t ad_len = data[off];
        if (ad_len == 0 || off + ad_len >= len + 0) {
            /* ad_len 不含自身；合法时 off+1+ad_len <= len */
        }
        if (off + 1 + ad_len > len) {
            break;
        }
        uint8_t ad_type = data[off + 1];
        const uint8_t *payload = data + off + 2; /* after type */
        uint8_t plen = (uint8_t)(ad_len - 1);

        if (ad_type == AD_TYPE_SERVICE16 && plen >= 2) {
            uint16_t uuid = (uint16_t)(payload[0] | (payload[1] << 8));
            if (uuid == UUID16_ENV_SENSE) {
                const uint8_t *after_uuid = payload + 2;
                uint16_t after_len = (uint16_t)(plen - 2);
                const uint8_t *ad_elem = data + off; /* [size][type][...] */

                atc_ble_sample_t s;
                bool ok = false;
                /* 明文：UUID 后 15 字节（MAC6+T2+H2+mV2+batt+cnt+flags），AD size=18(0x12)
                 * 加密：UUID 后约 11 字节（codec0+cipher+mic），AD size=14(0x0e) */
                bool looks_clear = (after_len >= 15);
                bool looks_enc = (s_has_bindkey && after_len >= 11);

                if (looks_clear && after_len >= 15) {
                    ok = atc_ble_parse_pvvx_clear(after_uuid, after_len, addr_msb, &s);
                }
                if (!ok && looks_enc && s_has_bindkey) {
                    ok = atc_ble_parse_pvvx_encrypted(ad_elem, (uint16_t)(1 + ad_len),
                                                      addr_msb, s_bindkey, &s);
                }
                if (ok) {
                    memcpy(s.mac, addr_msb, 6);
                    s.rssi = rssi;
                    cache_store(&s);
                    ESP_LOGD(TAG, "ATC帧 t=%.2f h=%.2f batt=%u%% rssi=%d mac=%02X:%02X:%02X:%02X:%02X:%02X",
                             (double)s.temperature, (double)s.humidity,
                             s.battery_pct == 0xFF ? 0 : s.battery_pct,
                             (int)rssi,
                             addr_msb[0], addr_msb[1], addr_msb[2],
                             addr_msb[3], addr_msb[4], addr_msb[5]);
                } else {
                    ESP_LOGD(TAG, "0x181A 未解析 mac=%02X:%02X:%02X:%02X:%02X:%02X after=%u",
                             addr_msb[0], addr_msb[1], addr_msb[2],
                             addr_msb[3], addr_msb[4], addr_msb[5],
                             after_len);
                }
            }
        }
        off += (size_t)ad_len + 1;
    }
}

/** NimBLE 地址：val[0]=LSB → 显示序 MSB */
static void addr_to_msb(const ble_addr_t *addr, uint8_t msb[6])
{
    for (int i = 0; i < 6; i++) {
        msb[i] = addr->val[5 - i];
    }
}

static int gap_on_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (event == NULL) {
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        uint8_t msb[6];
        addr_to_msb(&event->disc.addr, msb);
        handle_adv_raw(msb, event->disc.rssi, event->disc.data, event->disc.length_data);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        /* 禁止在 GAP 回调里调 ble_gap_disc（易与 host 死锁）；由 sup 任务续扫 */
        ESP_LOGD(TAG, "DISC_COMPLETE reason=%d", event->disc_complete.reason);
        s_scanning = false;
        return 0;
    default:
        return 0;
    }
}

static void start_scan_locked(void)
{
    if (!s_synced || !s_scan_wanted) {
        return;
    }
    if (s_scanning) {
        return;
    }
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.passive = 1;
    p.filter_duplicates = 0;
    p.itvl = 0;
    p.window = 0;
    p.filter_policy = 0;
    p.limited = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_on_event, NULL);
    if (rc == 0) {
        s_scanning = true;
        if (!s_scan_announced) {
            s_scan_announced = true;
            ESP_LOGI(TAG, "ATC BLE 扫描开始（周期窗口模式）");
        } else {
            ESP_LOGD(TAG, "ATC BLE 扫描恢复");
        }
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    }
}

/** 窗口逻辑截止：墙钟超过 close_ms 即停扫（数据面 ring 仍可被 pop） */
static bool window_expired_by_wallclock(void)
{
    if (!s_window_active) {
        return false;
    }
    return (esp_timer_get_time() / 1000) > s_window_close_ms;
}

/** 到点后关窗停扫；幂等，sched / resume / scan_sup 均可调用 */
static void window_expire_if_due(void)
{
    if (!window_expired_by_wallclock()) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_window_active = false;
    portEXIT_CRITICAL(&s_lock);
    s_scan_wanted = false;
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
        ESP_LOGD(TAG, "ATC BLE 窗口墙钟到点，扫描停止");
    }
}

/** 独立任务负责启停扫描，避免在 NimBLE 回调里调 GAP API；仅窗口内续扫 */
static void scan_sup_task(void *arg)
{
    (void)arg;
    int beat = 0;
    for (;;) {
        window_expire_if_due();
        if (s_inited && s_synced && s_scan_wanted && !s_scanning) {
            start_scan_locked();
        }
        beat++;
        if (beat >= 15) { /* ~30s @ 2s */
            beat = 0;
            atc_ble_sample_t s;
            bool ok = atc_ble_pop_latest(&s);
            portENTER_CRITICAL(&s_lock);
            size_t win_n = s_win_count;
            size_t win_h = s_win_head;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "ATC心跳 scan=%d wanted=%d window=%d win_n=%u cache=%s age=%lldms heap=%u",
                     (int)s_scanning, (int)s_scan_wanted,
                     (int)s_window_active, (unsigned)win_n,
                     ok ? "ok" : "none",
                     ok ? (long long)((esp_timer_get_time() / 1000) - s.ts_ms) : -1LL,
                     (unsigned)esp_get_free_heap_size());
            (void)win_h;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGW(TAG, "ensure_addr rc=%d", rc);
    }
    s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    s_synced = true;
    ESP_LOGI(TAG, "NimBLE synced");
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset reason=%d", reason);
    s_synced = false;
    s_scanning = false;
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ---------------- public API ---------------- */

esp_err_t atc_ble_init(const uint8_t expect_mac[6], const uint8_t bindkey[16])
{
    if (s_inited) {
        return ESP_OK;
    }
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest.battery_pct = 0xFF;
    s_has_mac_filter = false;
    s_has_bindkey = false;
    if (expect_mac != NULL) {
        uint8_t z[6] = {0};
        if (!mac_eq(expect_mac, z)) {
            memcpy(s_expect_mac, expect_mac, 6);
            s_has_mac_filter = true;
        }
    }
    if (bindkey != NULL) {
        uint8_t z[16] = {0};
        if (memcmp(bindkey, z, 16) != 0) {
            memcpy(s_bindkey, bindkey, 16);
            s_has_bindkey = true;
        }
    }

    ESP_LOGI(TAG, "ATC BLE init  mac_filter=%d bindkey=%d（窗口扫描，默认停扫）",
             (int)s_has_mac_filter, (int)s_has_bindkey);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    nimble_port_freertos_init(host_task);

    BaseType_t ok = xTaskCreate(scan_sup_task, "atc_scan", 3072, NULL, 4, NULL);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "scan_sup_task 创建失败，扫描可能无法自动续启");
    }
    /* 默认不扫：等周期窗口 atc_ble_window_open */
    s_scan_wanted = false;
    s_inited = true;
    return ESP_OK;
}

void atc_ble_window_open(int64_t ref_ms, int64_t open_before_ms, int64_t close_after_ms)
{
    if (!s_inited) {
        return;
    }
    const int64_t open_ms = ref_ms - open_before_ms;
    const int64_t close_ms = ref_ms + close_after_ms;

    portENTER_CRITICAL(&s_lock);
    s_win_head = 0;
    s_win_count = 0;
    memset((void *)s_win_ring, 0, sizeof(s_win_ring));
    s_window_open_ms = open_ms;
    s_window_close_ms = close_ms;
    s_window_active = true;
    portEXIT_CRITICAL(&s_lock);

    s_scan_wanted = true;
    ESP_LOGI(TAG, "ATC BLE 窗口开启 ref=%lld open=%lld close=%lld（前%lldms~后%lldms）",
             (long long)ref_ms, (long long)open_ms, (long long)close_ms,
             (long long)open_before_ms, (long long)close_after_ms);
    if (s_synced) {
        start_scan_locked();
    }
}

void atc_ble_window_close(void)
{
    portENTER_CRITICAL(&s_lock);
    s_window_active = false;
    portEXIT_CRITICAL(&s_lock);
    s_scan_wanted = false;
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
        ESP_LOGD(TAG, "ATC BLE 窗口关闭，扫描暂停");
    } else {
        ESP_LOGD(TAG, "ATC BLE 窗口关闭");
    }
}

bool atc_ble_pop_window_best(int64_t ref_ms, atc_ble_sample_t *out)
{
    if (out == NULL) {
        return false;
    }
    bool found = false;
    atc_ble_sample_t best;
    memset(&best, 0, sizeof(best));
    int64_t best_dist = INT64_MAX;

    portENTER_CRITICAL(&s_lock);
    size_t n = s_win_count;
    size_t head = s_win_head;
    /* 只取窗口时间范围内的帧，再选 |ts-ref| 最小 */
    for (size_t i = 0; i < n; i++) {
        size_t idx = (head + ATC_WINDOW_RING_N - 1 - i) % ATC_WINDOW_RING_N;
        const atc_ble_sample_t *cand = &s_win_ring[idx];
        if (!cand->valid) {
            continue;
        }
        if (cand->ts_ms < s_window_open_ms || cand->ts_ms > s_window_close_ms) {
            continue;
        }
        int64_t d = cand->ts_ms - ref_ms;
        if (d < 0) {
            d = -d;
        }
        if (!found || d < best_dist) {
            best = *cand;
            best_dist = d;
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (!found) {
        return false;
    }
    *out = best;
    return true;
}

esp_err_t atc_ble_stop_scan(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    s_scan_wanted = false;
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
        /* 例行停扫：HTTP perform 期间降低 C3 上 Wi-Fi/BLE 空口争用 */
        ESP_LOGD(TAG, "ATC BLE 扫描暂停（HTTP 共存）");
    }
    return ESP_OK;
}

/** HTTP 后恢复：仅当本周期窗口仍打开且墙钟未过 close 时续扫 */
esp_err_t atc_ble_resume_scan_if_wanted(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    if (!s_window_active) {
        return ESP_OK;
    }
    if (window_expired_by_wallclock()) {
        window_expire_if_due();
        return ESP_OK;
    }
    s_scan_wanted = true;
    if (s_synced) {
        start_scan_locked();
    }
    return ESP_OK;
}

bool atc_ble_pop_latest(atc_ble_sample_t *out)
{
    if (out == NULL) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_latest;
    bool ok = s_latest.valid;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void atc_ble_clear_cache(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_latest, 0, sizeof(s_latest));
    s_latest.battery_pct = 0xFF;
    portEXIT_CRITICAL(&s_lock);
}

bool atc_ble_is_scanning(void)
{
    return s_scanning;
}
