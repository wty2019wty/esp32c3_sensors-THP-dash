#include "thp_queue.h"

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

#include "thp_config.h"
#include "thp_types.h"

static const char *TAG = "thp.queue";

#ifndef THP_OFFLINE_QUEUE_LEN
#define THP_OFFLINE_QUEUE_LEN 16
#endif

#define THP_QUEUE_MAGIC 0x54485131u /* "THQ1" */

/* RTC 慢速内存：deep sleep 保持，断电/上电复位清零 */
RTC_DATA_ATTR static uint32_t s_q_magic;
RTC_DATA_ATTR static thp_reading_t s_offline_q[THP_OFFLINE_QUEUE_LEN];
RTC_DATA_ATTR static size_t s_offline_head;
RTC_DATA_ATTR static size_t s_offline_count;

esp_err_t thp_queue_init(void)
{
    if (s_q_magic != THP_QUEUE_MAGIC) {
        s_offline_head = 0;
        s_offline_count = 0;
        s_q_magic = THP_QUEUE_MAGIC;
        ESP_LOGI(TAG, "RTC 队列冷启动清空（容量 %d）", THP_OFFLINE_QUEUE_LEN);
    } else if (s_offline_count > 0) {
        ESP_LOGI(TAG, "RTC 队列已恢复 %u/%d 条", (unsigned)s_offline_count,
                 THP_OFFLINE_QUEUE_LEN);
    }
    return ESP_OK;
}

void thp_queue_push(const thp_reading_t *r)
{
    if (r == NULL) {
        return;
    }
    if (s_offline_count >= THP_OFFLINE_QUEUE_LEN) {
        s_offline_head = (s_offline_head + 1) % THP_OFFLINE_QUEUE_LEN;
        s_offline_count--;
        ESP_LOGW(TAG, "离线队列已满(>%d)，丢弃最旧一条", THP_OFFLINE_QUEUE_LEN);
    }
    size_t idx = (s_offline_head + s_offline_count) % THP_OFFLINE_QUEUE_LEN;
    s_offline_q[idx] = *r;
    s_offline_count++;
    ESP_LOGW(TAG, "已入离线队列 kind=%s (%u/%u) iso=%s%s%s",
             thp_kind_tag(r->kind), (unsigned)s_offline_count,
             (unsigned)THP_OFFLINE_QUEUE_LEN,
             r->has_iso ? r->iso : "(no-ts)",
             r->has_th ? " TH" : "",
             r->has_p ? " P" : "");
}

bool thp_queue_peek_copy(thp_reading_t *out)
{
    if (out == NULL || s_offline_count == 0) {
        return false;
    }
    *out = s_offline_q[s_offline_head];
    return true;
}

void thp_queue_pop(void)
{
    if (s_offline_count > 0) {
        s_offline_head = (s_offline_head + 1) % THP_OFFLINE_QUEUE_LEN;
        s_offline_count--;
    }
}

void thp_queue_clear(void)
{
    s_offline_head = 0;
    s_offline_count = 0;
}

unsigned thp_queue_count(void)
{
    return (unsigned)s_offline_count;
}
