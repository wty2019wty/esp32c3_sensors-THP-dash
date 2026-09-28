/*
 * atc_ble — 多设备 pvvx ATC_MiThermometer / BTHome v2 被动扫描
 *
 * 支持：
 *   1) PVVX (Custom) 明文  — Service Data UUID 0x181A
 *   2) PVVX (Custom) 加密  — AES-CCM + 每设备 BindKey（AtcMiCodec）
 *   3) BTHome v2 明文      — UUID 0xFCD2
 *   4) BTHome v2 加密      — AES-CCM + 每设备 BindKey
 *
 * 多设备：按广播 MAC 匹配设备表；窗口环缓存带 dev_index。
 */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
#define UUID16_BTHOME      0xFCD2
#define AD_TYPE_SERVICE16  0x16

#define BTHOME_DEV_INFO_VER2       0x40
#define BTHOME_DEV_INFO_ENCRYPTED  0x01
#define BTHOME_DEV_INFO_VER_MASK   0xE0

#define BTHOME_OBJ_PACKET_ID   0x00
#define BTHOME_OBJ_BATTERY     0x01
#define BTHOME_OBJ_TEMPERATURE 0x02
#define BTHOME_OBJ_HUMIDITY    0x03
#define BTHOME_OBJ_HUMIDITY_U8 0x2E
#define BTHOME_OBJ_VOLTAGE     0x0C

static atc_ble_device_t s_devs[ATC_BLE_MAX_DEVICES];
static size_t s_ndev;
static atc_ble_sample_t s_latest[ATC_BLE_MAX_DEVICES];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* BTHome 加密防重放：每设备 counter */
static uint32_t s_bthome_counter[ATC_BLE_MAX_DEVICES];
static bool s_bthome_counter_valid[ATC_BLE_MAX_DEVICES];

static volatile bool s_scanning;
static volatile bool s_inited;
static volatile bool s_synced;
static volatile bool s_scan_wanted;
static bool s_scan_announced;
static uint8_t s_own_addr_type;
static SemaphoreHandle_t s_scan_mtx;

#define ATC_WINDOW_RING_N 48
static atc_ble_sample_t s_win_ring[ATC_WINDOW_RING_N];
static volatile size_t s_win_count;
static volatile size_t s_win_head;
static volatile bool s_window_active;
static volatile int64_t s_window_open_ms;
static volatile int64_t s_window_close_ms;
static int64_t s_window_open_before_ms;
static int64_t s_window_close_after_ms;

static int gap_on_event(struct ble_gap_event *event, void *arg);
static void start_scan_locked(void);
static void scan_sup_task(void *arg);

static void scan_lock(void)
{
    if (s_scan_mtx) {
        xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
    }
}

static void scan_unlock(void)
{
    if (s_scan_mtx) {
        xSemaphoreGive(s_scan_mtx);
    }
}

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

/** 表内查找；未命中返回 -1 */
static int find_dev(const uint8_t mac[6])
{
    if (mac == NULL || s_ndev == 0) {
        return -1;
    }
    for (size_t i = 0; i < s_ndev; i++) {
        if (mac_eq(s_devs[i].mac, mac)) {
            return (int)i;
        }
    }
    return -1;
}

static bool ccm_decrypt_tag4(const uint8_t key[16],
                             const uint8_t *nonce, size_t nonce_len,
                             const uint8_t *aad, size_t aad_len,
                             const uint8_t *ct_and_tag, size_t ct_tag_len,
                             uint8_t *plain, size_t plain_cap, size_t *plain_len)
{
    if (ct_tag_len < 4) {
        return false;
    }

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
    st = psa_import_key(&attr, key, 16, &key_id);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_import_key: %d", (int)st);
        return false;
    }

    size_t out_len = 0;
    st = psa_aead_decrypt(key_id, PSA_ALG_AEAD_WITH_SHORTENED_TAG(PSA_ALG_CCM, 4),
                          nonce, nonce_len,
                          aad, aad_len,
                          ct_and_tag, ct_tag_len,
                          plain, plain_cap, &out_len);
    psa_destroy_key(key_id);
    if (st != PSA_SUCCESS) {
        ESP_LOGD(TAG, "psa_aead_decrypt: %d (bindkey/MAC/密文不符)", (int)st);
        return false;
    }
    if (plain_len != NULL) {
        *plain_len = out_len;
    }
    return true;
}

static void cache_store(size_t dev_index, const atc_ble_sample_t *s)
{
    if (dev_index >= ATC_BLE_MAX_DEVICES) {
        return;
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    s_latest[dev_index] = *s;
    s_latest[dev_index].valid = true;
    s_latest[dev_index].ts_ms = now_ms;
    s_latest[dev_index].dev_index = (uint8_t)dev_index;
    if (s_window_active &&
        now_ms >= s_window_open_ms && now_ms <= s_window_close_ms) {
        s_win_ring[s_win_head] = s_latest[dev_index];
        s_win_head = (s_win_head + 1) % ATC_WINDOW_RING_N;
        if (s_win_count < ATC_WINDOW_RING_N) {
            s_win_count++;
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

/* ---------------- parsers（无全局 MAC 过滤，由 handle_adv 表驱动） ---------------- */

bool atc_ble_parse_pvvx_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                              const uint8_t adv_mac[6], atc_ble_sample_t *out)
{
    if (after_uuid == NULL || out == NULL || after_uuid_len < 15) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->battery_pct = 0xFF;

    for (int i = 0; i < 6; i++) {
        out->mac[i] = after_uuid[5 - i];
    }
    if (adv_mac != NULL) {
        memcpy(out->mac, adv_mac, 6);
    }

    int16_t t100 = (int16_t)(after_uuid[6] | (after_uuid[7] << 8));
    uint16_t h100 = (uint16_t)(after_uuid[8] | (after_uuid[9] << 8));
    out->temperature = t100 / 100.0f;
    out->humidity = h100 / 100.0f;
    out->battery_mv = (uint16_t)(after_uuid[10] | (after_uuid[11] << 8));
    out->battery_pct = after_uuid[12];
    out->adv_counter = after_uuid[13];
    out->flags = after_uuid[14];

    return th_range_ok(out->temperature, out->humidity);
}

bool atc_ble_parse_pvvx_encrypted(const uint8_t *ad, uint16_t ad_len,
                                  const uint8_t adv_mac[6],
                                  const uint8_t bindkey[16],
                                  atc_ble_sample_t *out)
{
    if (ad == NULL || out == NULL || adv_mac == NULL || bindkey == NULL) {
        return false;
    }
    if (ad_len < 4 + 1 + 6 + 4) {
        return false;
    }
    if (ad[1] != AD_TYPE_SERVICE16 || ad[2] != 0x1A || ad[3] != 0x18) {
        return false;
    }
    const uint8_t *codec = ad + 4;
    uint16_t codec_len = (uint16_t)(ad_len - 4);
    if (codec_len < 1 + 6 + 4) {
        return false;
    }

    uint8_t nonce[11];
    for (int i = 0; i < 6; i++) {
        nonce[i] = adv_mac[5 - i];
    }
    memcpy(nonce + 6, ad, 4);
    nonce[10] = codec[0];

    const uint8_t *cipher = codec + 1;
    size_t cipher_len = codec_len - 1 - 4;
    const uint8_t *mic = codec + codec_len - 4;
    if (cipher_len > 16 || cipher_len < 6) {
        return false;
    }

    uint8_t ct_and_tag[20];
    if (cipher_len + 4 > sizeof(ct_and_tag)) {
        return false;
    }
    memcpy(ct_and_tag, cipher, cipher_len);
    memcpy(ct_and_tag + cipher_len, mic, 4);

    const uint8_t aad = 0x11;
    uint8_t plain[16];
    size_t plain_len = 0;
    if (!ccm_decrypt_tag4(bindkey, nonce, sizeof(nonce), &aad, 1,
                          ct_and_tag, cipher_len + 4,
                          plain, sizeof(plain), &plain_len)) {
        return false;
    }
    if (plain_len < 6) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->battery_pct = 0xFF;
    memcpy(out->mac, adv_mac, 6);
    int16_t t100 = (int16_t)(plain[0] | (plain[1] << 8));
    uint16_t h100 = (uint16_t)(plain[2] | (plain[3] << 8));
    out->temperature = t100 / 100.0f;
    out->humidity = h100 / 100.0f;
    out->battery_pct = plain[4];
    out->flags = plain[5];
    return th_range_ok(out->temperature, out->humidity);
}

static bool bthome_parse_objects(const uint8_t *objs, uint16_t objs_len,
                                 atc_ble_sample_t *out)
{
    if (objs == NULL || out == NULL) {
        return false;
    }
    bool has_t = false;
    bool has_h = false;
    size_t off = 0;
    while (off < objs_len) {
        uint8_t id = objs[off++];
        uint8_t vsz;
        switch (id) {
        case BTHOME_OBJ_PACKET_ID:
        case BTHOME_OBJ_BATTERY:
        case BTHOME_OBJ_HUMIDITY_U8:
            vsz = 1;
            break;
        case BTHOME_OBJ_TEMPERATURE:
        case BTHOME_OBJ_HUMIDITY:
        case BTHOME_OBJ_VOLTAGE:
            vsz = 2;
            break;
        default:
            off = objs_len;
            continue;
        }
        if (off + vsz > objs_len) {
            break;
        }
        const uint8_t *v = objs + off;
        switch (id) {
        case BTHOME_OBJ_PACKET_ID:
            out->adv_counter = v[0];
            break;
        case BTHOME_OBJ_BATTERY:
            out->battery_pct = v[0];
            break;
        case BTHOME_OBJ_TEMPERATURE:
            out->temperature = (int16_t)(v[0] | (v[1] << 8)) / 100.0f;
            has_t = true;
            break;
        case BTHOME_OBJ_HUMIDITY:
            out->humidity = (uint16_t)(v[0] | (v[1] << 8)) / 100.0f;
            has_h = true;
            break;
        case BTHOME_OBJ_HUMIDITY_U8:
            out->humidity = (float)v[0];
            has_h = true;
            break;
        case BTHOME_OBJ_VOLTAGE:
            out->battery_mv = (uint16_t)(v[0] | (v[1] << 8));
            break;
        default:
            break;
        }
        off += vsz;
    }
    return has_t && has_h && th_range_ok(out->temperature, out->humidity);
}

bool atc_ble_parse_bthome_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                                const uint8_t adv_mac[6], atc_ble_sample_t *out)
{
    if (after_uuid == NULL || out == NULL || after_uuid_len < 1) {
        return false;
    }
    uint8_t info = after_uuid[0];
    if ((info & BTHOME_DEV_INFO_ENCRYPTED) != 0) {
        return false;
    }
    if ((info & BTHOME_DEV_INFO_VER_MASK) != BTHOME_DEV_INFO_VER2) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->battery_pct = 0xFF;
    if (adv_mac != NULL) {
        memcpy(out->mac, adv_mac, 6);
    }
    return bthome_parse_objects(after_uuid + 1, (uint16_t)(after_uuid_len - 1), out);
}

bool atc_ble_parse_bthome_encrypted(const uint8_t *ad, uint16_t ad_len,
                                    const uint8_t adv_mac[6],
                                    const uint8_t bindkey[16],
                                    atc_ble_sample_t *out)
{
    if (ad == NULL || out == NULL || adv_mac == NULL || bindkey == NULL) {
        return false;
    }
    if (ad_len < 4 + 1 + 0 + 4 + 4) {
        return false;
    }
    if (ad[1] != AD_TYPE_SERVICE16 || ad[2] != 0xD2 || ad[3] != 0xFC) {
        return false;
    }
    const uint8_t *codec = ad + 4;
    uint16_t codec_len = (uint16_t)(ad_len - 4);
    if (codec_len < 1 + 4 + 4) {
        return false;
    }
    uint8_t info = codec[0];
    if ((info & BTHOME_DEV_INFO_ENCRYPTED) == 0) {
        return false;
    }
    if ((info & BTHOME_DEV_INFO_VER_MASK) != BTHOME_DEV_INFO_VER2) {
        return false;
    }

    size_t tail = 8;
    size_t cipher_len = (size_t)codec_len - 1 - tail;
    if (cipher_len < 2 || cipher_len > 16) {
        return false;
    }
    const uint8_t *cipher = codec + 1;
    const uint8_t *counter = codec + 1 + cipher_len;
    const uint8_t *mic = counter + 4;

    uint8_t nonce[13];
    memcpy(nonce, adv_mac, 6);
    nonce[6] = 0xD2;
    nonce[7] = 0xFC;
    nonce[8] = info;
    memcpy(nonce + 9, counter, 4);

    uint8_t ct_and_tag[20];
    memcpy(ct_and_tag, cipher, cipher_len);
    memcpy(ct_and_tag + cipher_len, mic, 4);

    uint8_t plain[16];
    size_t plain_len = 0;
    if (!ccm_decrypt_tag4(bindkey, nonce, sizeof(nonce), NULL, 0,
                          ct_and_tag, cipher_len + 4,
                          plain, sizeof(plain), &plain_len)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->battery_pct = 0xFF;
    memcpy(out->mac, adv_mac, 6);
    out->adv_counter = counter[0];
    if (!bthome_parse_objects(plain, (uint16_t)plain_len, out)) {
        return false;
    }

    uint32_t ctr = (uint32_t)counter[0] | ((uint32_t)counter[1] << 8) |
                   ((uint32_t)counter[2] << 16) | ((uint32_t)counter[3] << 24);
    int di = find_dev(adv_mac);
    if (di >= 0) {
        bool replay = s_bthome_counter_valid[di] && ctr <= s_bthome_counter[di];
        if (replay) {
            ESP_LOGD(TAG, "BTHome 加密帧 dev=%d counter=%lu 未递增，丢弃",
                     di, (unsigned long)ctr);
            return false;
        }
        s_bthome_counter[di] = ctr;
        s_bthome_counter_valid[di] = true;
    }
    return true;
}

static void handle_adv_raw(const uint8_t addr_msb[6], int8_t rssi,
                           const uint8_t *data, uint8_t len)
{
    if (data == NULL || len < 3 || s_ndev == 0) {
        return;
    }
    int di = find_dev(addr_msb);
    if (di < 0) {
        return;
    }
    const uint8_t *bindkey = s_devs[di].has_bindkey ? s_devs[di].bindkey : NULL;

    size_t off = 0;
    while (off + 1 < len) {
        uint8_t ad_len = data[off];
        if (ad_len == 0) {
            break;
        }
        if (off + 1 + ad_len > len) {
            break;
        }
        uint8_t ad_type = data[off + 1];
        const uint8_t *payload = data + off + 2;
        uint8_t plen = (uint8_t)(ad_len - 1);

        if (ad_type == AD_TYPE_SERVICE16 && plen >= 2) {
            uint16_t uuid = (uint16_t)(payload[0] | (payload[1] << 8));
            if (uuid == UUID16_ENV_SENSE || uuid == UUID16_BTHOME) {
                const uint8_t *after_uuid = payload + 2;
                uint16_t after_len = (uint16_t)(plen - 2);
                const uint8_t *ad_elem = data + off;

                atc_ble_sample_t s;
                bool ok = false;
                bool is_bthome = (uuid == UUID16_BTHOME);

                if (!is_bthome) {
                    bool looks_clear = (after_len >= 15);
                    bool looks_enc = (bindkey != NULL && after_len >= 11);
                    if (looks_clear) {
                        ok = atc_ble_parse_pvvx_clear(after_uuid, after_len, addr_msb, &s);
                    }
                    if (!ok && looks_enc) {
                        ok = atc_ble_parse_pvvx_encrypted(ad_elem, (uint16_t)(1 + ad_len),
                                                          addr_msb, bindkey, &s);
                    }
                } else {
                    bool looks_enc = (after_len >= 1 + 4 + 4) &&
                                     (after_uuid[0] & BTHOME_DEV_INFO_ENCRYPTED) != 0;
                    if (!looks_enc) {
                        ok = atc_ble_parse_bthome_clear(after_uuid, after_len, addr_msb, &s);
                    } else if (bindkey != NULL) {
                        ok = atc_ble_parse_bthome_encrypted(ad_elem, (uint16_t)(1 + ad_len),
                                                            addr_msb, bindkey, &s);
                    }
                }
                if (ok) {
                    memcpy(s.mac, addr_msb, 6);
                    s.rssi = rssi;
                    s.dev_index = (uint8_t)di;
                    cache_store((size_t)di, &s);
                    ESP_LOGD(TAG, "dev=%d %s帧 t=%.2f h=%.2f rssi=%d",
                             di, is_bthome ? "BTHome" : "ATC",
                             (double)s.temperature, (double)s.humidity, (int)rssi);
                }
            }
        }
        off += (size_t)ad_len + 1;
    }
}

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
            ESP_LOGI(TAG, "ATC BLE 扫描开始（%u 设备，窗口模式）", (unsigned)s_ndev);
        }
    } else if (rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc rc=%d", rc);
    }
}

/** 在持锁状态下调用：停扫并复位 scanning */
static void stop_scan_locked(void)
{
    if (s_scanning) {
        ble_gap_disc_cancel();
        s_scanning = false;
    }
}

static bool window_expired_by_wallclock(void)
{
    if (!s_window_active) {
        return false;
    }
    return (esp_timer_get_time() / 1000) > s_window_close_ms;
}

static void window_expire_if_due(void)
{
    if (!window_expired_by_wallclock()) {
        return;
    }
    scan_lock();
    portENTER_CRITICAL(&s_lock);
    s_window_active = false;
    portEXIT_CRITICAL(&s_lock);
    s_scan_wanted = false;
    stop_scan_locked();
    scan_unlock();
}

static void scan_sup_task(void *arg)
{
    (void)arg;
    int beat = 0;
    for (;;) {
        window_expire_if_due();
        scan_lock();
        if (s_inited && s_synced && s_scan_wanted && !s_scanning) {
            start_scan_locked();
        }
        scan_unlock();
        beat++;
        if (beat >= 15) {
            beat = 0;
            portENTER_CRITICAL(&s_lock);
            size_t win_n = s_win_count;
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "ATC心跳 scan=%d window=%d win_n=%u ndev=%u heap=%u",
                     (int)s_scanning, (int)s_window_active,
                     (unsigned)win_n, (unsigned)s_ndev,
                     (unsigned)esp_get_free_heap_size());
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

size_t atc_ble_device_count(void)
{
    return s_ndev;
}

esp_err_t atc_ble_init(const atc_ble_device_t *devs, size_t count)
{
    if (s_inited) {
        return ESP_OK;
    }
    if (devs == NULL || count == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (count > ATC_BLE_MAX_DEVICES) {
        count = ATC_BLE_MAX_DEVICES;
    }
    memcpy(s_devs, devs, count * sizeof(atc_ble_device_t));
    s_ndev = count;
    memset(s_latest, 0, sizeof(s_latest));
    memset(s_bthome_counter_valid, 0, sizeof(s_bthome_counter_valid));
    for (size_t i = 0; i < s_ndev; i++) {
        s_latest[i].battery_pct = 0xFF;
    }

    ESP_LOGI(TAG, "ATC BLE init  ndev=%u（窗口扫描，默认停扫）", (unsigned)s_ndev);

    if (s_scan_mtx == NULL) {
        s_scan_mtx = xSemaphoreCreateMutex();
        if (s_scan_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

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
        ESP_LOGW(TAG, "scan_sup_task 创建失败");
    }
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
    s_window_open_before_ms = open_before_ms;
    s_window_close_after_ms = close_after_ms;
    s_window_open_ms = open_ms;
    s_window_close_ms = close_ms;
    s_window_active = true;
    portEXIT_CRITICAL(&s_lock);

    scan_lock();
    s_scan_wanted = true;
    ESP_LOGI(TAG, "ATC BLE 窗口开启 ref=%lld open=%lld close=%lld（前%lldms~后%lldms）",
             (long long)ref_ms, (long long)open_ms, (long long)close_ms,
             (long long)open_before_ms, (long long)close_after_ms);
    if (s_synced) {
        start_scan_locked();
    }
    scan_unlock();
}

void atc_ble_window_realign(int64_t ref_ms)
{
    if (!s_inited) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (!s_window_active) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    s_window_open_ms = ref_ms - s_window_open_before_ms;
    s_window_close_ms = ref_ms + s_window_close_after_ms;
    portEXIT_CRITICAL(&s_lock);
}

void atc_ble_window_close(void)
{
    scan_lock();
    portENTER_CRITICAL(&s_lock);
    s_window_active = false;
    portEXIT_CRITICAL(&s_lock);
    s_scan_wanted = false;
    stop_scan_locked();
    scan_unlock();
    ESP_LOGD(TAG, "ATC BLE 窗口关闭，扫描暂停");
}

bool atc_ble_pop_window_best(size_t dev_index, int64_t ref_ms, atc_ble_sample_t *out)
{
    if (out == NULL || dev_index >= s_ndev) {
        return false;
    }
    bool found = false;
    atc_ble_sample_t best;
    memset(&best, 0, sizeof(best));
    int64_t best_dist = INT64_MAX;

    portENTER_CRITICAL(&s_lock);
    size_t n = s_win_count;
    size_t head = s_win_head;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (head + ATC_WINDOW_RING_N - 1 - i) % ATC_WINDOW_RING_N;
        const atc_ble_sample_t *cand = &s_win_ring[idx];
        if (!cand->valid || cand->dev_index != (uint8_t)dev_index) {
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
    scan_lock();
    s_scan_wanted = false;
    stop_scan_locked();
    scan_unlock();
    return ESP_OK;
}

esp_err_t atc_ble_resume_scan_if_wanted(void)
{
    if (!s_inited) {
        return ESP_OK;
    }
    scan_lock();
    if (!s_window_active) {
        scan_unlock();
        return ESP_OK;
    }
    if (window_expired_by_wallclock()) {
        scan_unlock();
        window_expire_if_due();
        return ESP_OK;
    }
    s_scan_wanted = true;
    if (s_synced) {
        start_scan_locked();
    }
    scan_unlock();
    return ESP_OK;
}

bool atc_ble_pop_latest(size_t dev_index, atc_ble_sample_t *out)
{
    if (out == NULL || dev_index >= ATC_BLE_MAX_DEVICES) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_latest[dev_index];
    bool ok = s_latest[dev_index].valid;
    portEXIT_CRITICAL(&s_lock);
    return ok;
}

void atc_ble_clear_cache(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(s_latest, 0, sizeof(s_latest));
    for (size_t i = 0; i < ATC_BLE_MAX_DEVICES; i++) {
        s_latest[i].battery_pct = 0xFF;
    }
    portEXIT_CRITICAL(&s_lock);
}

bool atc_ble_is_scanning(void)
{
    return s_scanning;
}
