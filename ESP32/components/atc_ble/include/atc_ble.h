/*
 * atc_ble — pvvx ATC / PVVX(Custom) / BTHome v2 BLE 被动扫描与解析
 * PVVX 明文：Service Data UUID 0x181A，MAC+T/H+电量
 * PVVX 加密：同 UUID，AES-CCM + BindKey（pvvx AtcMiCodec，AAD=0x11）
 * BTHome v2 明文/加密：Service Data UUID 0xFCD2，object 流 / AES-CCM + BindKey
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    float    temperature;   /* °C */
    float    humidity;      /* %RH */
    uint16_t battery_mv;    /* 0 = 未知 */
    uint8_t  battery_pct;   /* 0xFF = 未知 */
    uint8_t  adv_counter;
    uint8_t  flags;
    int8_t   rssi;
    int64_t  ts_ms;         /* esp_timer，缓存新鲜度 */
    uint8_t  mac[6];        /* 显示序 MSB first，如 A4:C1:38:… */
    bool     valid;
} atc_ble_sample_t;

/** expect_mac 为显示序 6 字节；全 0 表示不按 MAC 过滤（首帧锁定） */
esp_err_t atc_ble_init(const uint8_t expect_mac[6], const uint8_t bindkey[16]);

/** 窗口扫描：仅在 [ref-open, ref+close] 帧才会进入窗口环形缓存 */
void atc_ble_window_open(int64_t ref_ms, int64_t open_before_ms, int64_t close_after_ms);
/** 周期实际 T 与开窗估算偏差时，按同一 ref 重算窗边界；不清环缓存、不改扫描态 */
void atc_ble_window_realign(int64_t ref_ms);
/** 关闭窗口并停扫（窗口外不扫描） */
void atc_ble_window_close(void);
/** 取窗口内 |ts-ref| 最小的一帧；无样本返回 false */
bool atc_ble_pop_window_best(int64_t ref_ms, atc_ble_sample_t *out);

esp_err_t atc_ble_stop_scan(void);
/** HTTP 后恢复：窗口仍打开且墙钟未过 close 才续扫 */
esp_err_t atc_ble_resume_scan_if_wanted(void);
bool      atc_ble_pop_latest(atc_ble_sample_t *out);
void      atc_ble_clear_cache(void);
bool      atc_ble_is_scanning(void);

/** 解析 "A4:C1:38:E2:4E:43" / "A4C138E24E43" → 显示序 6 字节 */
bool atc_ble_parse_mac_str(const char *s, uint8_t out[6]);
/** 解析 32 位 hex BindKey */
bool atc_ble_parse_key_hex(const char *s, uint8_t out[16]);

/**
 * @brief 解析 pvvx Custom 明文 service-data payload（UUID 之后）
 * @param after_uuid  UUID 后的数据
 * @param after_uuid_len
 * @param adv_mac     广播地址（显示序 MSB first）
 */
bool atc_ble_parse_pvvx_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                              const uint8_t adv_mac[6], atc_ble_sample_t *out);

/**
 * @brief 解析 pvvx Custom 加密帧（pvvx AtcMiCodec / AES-CCM）
 * @param ad          完整 AD：size, 0x16, 0x1A, 0x18, codec...
 * @param ad_len
 * @param adv_mac     广播地址（显示序 MSB first）
 * @param bindkey     16 字节
 */
bool atc_ble_parse_pvvx_encrypted(const uint8_t *ad, uint16_t ad_len,
                                  const uint8_t adv_mac[6],
                                  const uint8_t bindkey[16],
                                  atc_ble_sample_t *out);

/**
 * @brief 解析 BTHome v2 明文 service-data payload（UUID 0xFCD2 之后）
 * @param after_uuid  device_info + object 流
 */
bool atc_ble_parse_bthome_clear(const uint8_t *after_uuid, uint16_t after_uuid_len,
                                const uint8_t adv_mac[6], atc_ble_sample_t *out);

/**
 * @brief 解析 BTHome v2 加密帧（AES-CCM，无 AAD，counter 在密文后）
 * @param ad          完整 AD：size, 0x16, 0xD2, 0xFC, device_info, cipher, counter, mic
 * @param bindkey     16 字节
 */
bool atc_ble_parse_bthome_encrypted(const uint8_t *ad, uint16_t ad_len,
                                    const uint8_t adv_mac[6],
                                    const uint8_t bindkey[16],
                                    atc_ble_sample_t *out);
