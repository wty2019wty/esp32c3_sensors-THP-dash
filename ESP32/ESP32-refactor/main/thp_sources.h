/*
 * 多源注册表：source 0 = LOCAL I2C，source 1..N = BLE 设备
 *
 * Token / device_id / 名称在上报与补传时按 source_id 解析。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "thp_types.h"

/** 建表并解析 BLE MAC/BindKey；可重复调用（仅第一次生效） */
esp_err_t thp_sources_init(void);

size_t thp_source_count(void);
thp_src_kind_t thp_source_kind(uint8_t source_id);
const char *thp_source_name(uint8_t source_id);
const char *thp_source_token(uint8_t source_id);
const char *thp_source_device_id(uint8_t source_id);

/** BLE 源在 atc_ble 设备表中的下标；非 BLE 源返回 -1 */
int thp_source_ble_index(uint8_t source_id);

/** 源 id 是否有效且 Token 可用 */
bool thp_source_ready(uint8_t source_id);

/** 打印源表摘要 */
void thp_sources_dump(void);
