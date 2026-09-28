/*
 * atc_ble — 多设备 pvvx ATC / BTHome v2 被动扫描
 *
 * 设备表驱动：按广播 MAC 匹配，加密帧使用对应 BindKey。
 * 窗口环缓存带 dev_index，可按设备取距 ref 最近的一帧。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ATC_BLE_MAX_DEVICES 8

typedef struct {
    float    temperature;
    float    humidity;
    uint16_t battery_mv;
    uint8_t  battery_pct;   /* 0xFF = 未知 */
    uint8_t  adv_counter;
    uint8_t  flags;
    int8_t   rssi;
    int64_t  ts_ms;
    uint8_t  mac[6];
    uint8_t  dev_index;
    bool     valid;
} atc_ble_sample_t;

typedef struct {
    uint8_t mac[6];       /* 显示序 MSB first */
    uint8_t bindkey[16];  /* 全 0 = 仅明文 */
    bool    has_bindkey;
} atc_ble_device_t;

/** devs 为多设备表；count>0 时仅接受表内 MAC */
esp_err_t atc_ble_init(const atc_ble_device_t *devs, size_t count);

void atc_ble_window_open(int64_t ref_ms, int64_t open_before_ms, int64_t close_after_ms);
void atc_ble_window_realign(int64_t ref_ms);
void atc_ble_window_close(void);

/** 取窗口内该设备 |ts-ref| 最小的一帧 */
bool atc_ble_pop_window_best(size_t dev_index, int64_t ref_ms, atc_ble_sample_t *out);

esp_err_t atc_ble_stop_scan(void);
esp_err_t atc_ble_resume_scan_if_wanted(void);
bool      atc_ble_pop_latest(size_t dev_index, atc_ble_sample_t *out);
void      atc_ble_clear_cache(void);
bool      atc_ble_is_scanning(void);
size_t    atc_ble_device_count(void);

bool atc_ble_parse_mac_str(const char *s, uint8_t out[6]);
bool atc_ble_parse_key_hex(const char *s, uint8_t out[16]);

bool atc_ble_parse_pvvx_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                              const uint8_t adv_mac[6], atc_ble_sample_t *out);
bool atc_ble_parse_pvvx_encrypted(const uint8_t *ad, uint16_t ad_len,
                                  const uint8_t adv_mac[6],
                                  const uint8_t bindkey[16],
                                  atc_ble_sample_t *out);
bool atc_ble_parse_bthome_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                                const uint8_t adv_mac[6], atc_ble_sample_t *out);
bool atc_ble_parse_bthome_encrypted(const uint8_t *ad, uint16_t ad_len,
                                    const uint8_t adv_mac[6],
                                    const uint8_t bindkey[16],
                                    atc_ble_sample_t *out);
