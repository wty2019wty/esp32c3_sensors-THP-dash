#pragma once

#include <stdbool.h>
#include <time.h>

#include "thp_types.h"

bool thp_time_is_synced(void);
void thp_time_sntp_start(void);
bool thp_time_format_iso_at(time_t now, char *out);
bool thp_time_format_iso(char *out);
bool thp_time_sync_before_report(void);
void thp_time_stamp_reading(thp_reading_t *r);
