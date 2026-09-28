#pragma once

#include <stdbool.h>
#include <time.h>

#include "thp_types.h"

bool thp_time_is_synced(void);
void thp_time_sntp_start(void);
bool thp_time_format_iso_at(time_t now, char *out);
bool thp_time_format_iso(char *out);
bool thp_time_sync_before_report(void);

/** 清零 reading 并填 Wi-Fi RSSI（不写 iso） */
void thp_time_stamp_reading(thp_reading_t *r);

/**
 * 按采样单调时刻回填 iso（采样时刻 UTC，而非组帧时刻）。
 * sample_mono_ms 为 esp_timer_get_time()/1000；NTP 未就绪则 has_iso=false。
 */
bool thp_time_fill_sample_iso(thp_reading_t *r, int64_t sample_mono_ms);
