/*
 * SNTP 校时与 ISO-8601 UTC
 */
#pragma once

#include <stdbool.h>
#include <time.h>

#include "thp_types.h"

void thp_time_sntp_start(void);
bool thp_time_is_synced(void);

/**
 * @brief 每次上报前同步系统时间
 * @return true 可写 measured_at（本次成功，或失败但已有可信时间）
 */
bool thp_time_sync_before_report(void);

bool thp_time_format_iso_at(time_t now, char *out);
bool thp_time_format_iso(char *out);

/** 采样时刻打 RSSI + UTC 戳 */
void thp_time_stamp_reading(thp_reading_t *r);
