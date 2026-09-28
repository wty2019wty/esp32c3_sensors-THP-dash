#include "thp_sources.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "atc_ble.h"
#include "thp_config.h"

static const char *TAG = "thp.sources";

#define THP_MAX_SOURCES (1 + ATC_BLE_MAX_DEVICES)

typedef struct {
    thp_src_kind_t kind;
    const char *name;
    const char *token;
    const char *device_id;
    int ble_index; /* BLE 在 atc_ble 表中的下标，LOCAL 为 -1 */
    bool ready;
} source_slot_t;

static source_slot_t s_src[THP_MAX_SOURCES];
static size_t s_count;
static bool s_inited;

#ifndef THP_BLE_ENABLE
#define THP_BLE_ENABLE 0
#endif

static bool token_ok(const char *t)
{
    if (t == NULL || t[0] == '\0') {
        return false;
    }
    /* 占位符统一以 thp_replace_me 开头 */
    if (strncmp(t, "thp_replace_me", 14) == 0) {
        return false;
    }
    return strlen(t) >= 8;
}

esp_err_t thp_sources_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    s_src[0].kind = THP_SRC_LOCAL;
    s_src[0].name = "local";
    s_src[0].token = THP_LOCAL_TOKEN;
    s_src[0].device_id = THP_LOCAL_DEVICE_ID;
    s_src[0].ble_index = -1;
    s_src[0].ready = token_ok(THP_LOCAL_TOKEN);
    if (!s_src[0].ready) {
        ESP_LOGW(TAG, "LOCAL Token 未配置或为占位符");
    }
    s_count = 1;

#if THP_BLE_ENABLE
    static const thp_ble_cfg_entry_t ble_cfg[] = THP_BLE_DEVICES;
    const size_t n = sizeof(ble_cfg) / sizeof(ble_cfg[0]);
    atc_ble_device_t devs[ATC_BLE_MAX_DEVICES];
    size_t ndev = 0;

    for (size_t i = 0; i < n && s_count < THP_MAX_SOURCES; i++) {
        uint8_t mac[6];
        uint8_t key[16];
        if (!atc_ble_parse_mac_str(ble_cfg[i].mac, mac)) {
            ESP_LOGW(TAG, "BLE[%u] '%s' MAC 非法: %s，跳过",
                     (unsigned)i, ble_cfg[i].name ? ble_cfg[i].name : "?",
                     ble_cfg[i].mac ? ble_cfg[i].mac : "(null)");
            continue;
        }
        bool key_ok = false;
        if (ble_cfg[i].bindkey != NULL && ble_cfg[i].bindkey[0] != '\0') {
            key_ok = atc_ble_parse_key_hex(ble_cfg[i].bindkey, key);
        }
        if (!key_ok) {
            memset(key, 0, sizeof(key));
        }

        memset(&devs[ndev], 0, sizeof(devs[ndev]));
        memcpy(devs[ndev].mac, mac, 6);
        memcpy(devs[ndev].bindkey, key, 16);
        devs[ndev].has_bindkey = key_ok;

        s_src[s_count].kind = THP_SRC_BLE;
        s_src[s_count].name = (ble_cfg[i].name && ble_cfg[i].name[0]) ? ble_cfg[i].name : "ble";
        s_src[s_count].token = ble_cfg[i].token ? ble_cfg[i].token : "";
        s_src[s_count].device_id = ble_cfg[i].device_id ? ble_cfg[i].device_id : "";
        s_src[s_count].ble_index = (int)ndev;
        s_src[s_count].ready = token_ok(s_src[s_count].token);
        if (!s_src[s_count].ready) {
            ESP_LOGW(TAG, "BLE[%u] '%s' Token 未配置", (unsigned)i, s_src[s_count].name);
        }
        s_count++;
        ndev++;
    }

    if (ndev > 0) {
        esp_err_t err = atc_ble_init(devs, ndev);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "atc_ble_init 失败: %s", esp_err_to_name(err));
        }
    }
#endif

    s_inited = true;
    thp_sources_dump();
    return ESP_OK;
}

size_t thp_source_count(void)
{
    return s_count;
}

thp_src_kind_t thp_source_kind(uint8_t source_id)
{
    if (source_id >= s_count) {
        return THP_SRC_LOCAL;
    }
    return s_src[source_id].kind;
}

const char *thp_source_name(uint8_t source_id)
{
    if (source_id >= s_count) {
        return "?";
    }
    return s_src[source_id].name;
}

const char *thp_source_token(uint8_t source_id)
{
    if (source_id >= s_count) {
        return "";
    }
    return s_src[source_id].token;
}

const char *thp_source_device_id(uint8_t source_id)
{
    if (source_id >= s_count) {
        return "";
    }
    return s_src[source_id].device_id;
}

int thp_source_ble_index(uint8_t source_id)
{
    if (source_id >= s_count) {
        return -1;
    }
    return s_src[source_id].ble_index;
}

bool thp_source_ready(uint8_t source_id)
{
    if (source_id >= s_count) {
        return false;
    }
    return s_src[source_id].ready;
}

void thp_source_set_ready(uint8_t source_id, bool ready)
{
    if (source_id >= s_count) {
        return;
    }
    if (s_src[source_id].ready == ready) {
        return;
    }
    s_src[source_id].ready = ready;
    ESP_LOGW(TAG, "源[%u] %s -> ready=%d", (unsigned)source_id, s_src[source_id].name,
             (int)ready);
}

void thp_sources_dump(void)
{
    ESP_LOGI(TAG, "源表: %u 路（local + %u BLE）", (unsigned)s_count,
             (unsigned)(s_count > 0 ? s_count - 1 : 0));
    for (size_t i = 0; i < s_count; i++) {
        ESP_LOGI(TAG, "  [%u] %s kind=%s ble_idx=%d token=%s ready=%d",
                 (unsigned)i, s_src[i].name,
                 s_src[i].kind == THP_SRC_LOCAL ? "LOCAL" : "BLE",
                 s_src[i].ble_index,
                 s_src[i].ready ? "ok" : "missing",
                 (int)s_src[i].ready);
    }
}
