/*
 * SNTP 校时与 ISO-8601 UTC
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "thp_types.h"

void thp_time_sntp_start(void);

/** 仅初始化 SNTP（不阻塞等待）；deep sleep 唤醒后调用，供周期性重同步使用 */
void thp_time_sntp_ensure(void);
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

/** deep sleep 前把当前 UTC（µs）与计划睡眠时长写入 RTC；醒来后 thp_time_rtc_restore */
void thp_time_rtc_save(int64_t planned_sleep_us);

/** 唤醒后用 RTC 里的时刻 + 计划睡眠时长恢复系统时间；无可信时间则返回 false */
bool thp_time_rtc_restore(void);
