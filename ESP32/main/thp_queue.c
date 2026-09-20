#include "thp_queue.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "thp_config.h"
#include "thp_types.h"

static const char *TAG = "thp.queue";

#ifndef THP_OFFLINE_QUEUE_LEN
#define THP_OFFLINE_QUEUE_LEN 288
#endif

static thp_reading_t s_offline_q[THP_OFFLINE_QUEUE_LEN];
static size_t s_offline_head;
static size_t s_offline_count;
static SemaphoreHandle_t s_q_mtx;

static void q_lock(void)
{
    if (s_q_mtx) {
        xSemaphoreTake(s_q_mtx, portMAX_DELAY);
    }
}

static void q_unlock(void)
{
    if (s_q_mtx) {
        xSemaphoreGive(s_q_mtx);
    }
}

esp_err_t thp_queue_init(void)
{
    s_q_mtx = xSemaphoreCreateMutex();
    if (s_q_mtx == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void thp_queue_push(const thp_reading_t *r)
{
    if (r == NULL) {
        return;
    }
    q_lock();
    if (s_offline_count >= THP_OFFLINE_QUEUE_LEN) {
        s_offline_head = (s_offline_head + 1) % THP_OFFLINE_QUEUE_LEN;
        s_offline_count--;
        ESP_LOGW(TAG, "离线队列已满(>%d)，丢弃最旧一条", THP_OFFLINE_QUEUE_LEN);
    }
    size_t idx = (s_offline_head + s_offline_count) % THP_OFFLINE_QUEUE_LEN;
    s_offline_q[idx] = *r;
    s_offline_count++;
    unsigned n = (unsigned)s_offline_count;
    q_unlock();
    ESP_LOGW(TAG, "已入离线队列 kind=%s (%u/%u) iso=%s%s%s",
             thp_kind_tag(r->kind), n, (unsigned)THP_OFFLINE_QUEUE_LEN,
             r->has_iso ? r->iso : "(no-ts)",
             r->has_th ? " TH" : "",
             r->has_p ? " P" : "");
}

bool thp_queue_peek_copy(thp_reading_t *out)
{
    if (out == NULL) {
        return false;
    }
    q_lock();
    if (s_offline_count == 0) {
        q_unlock();
        return false;
    }
    *out = s_offline_q[s_offline_head];
    q_unlock();
    return true;
}

void thp_queue_pop(void)
{
    q_lock();
    if (s_offline_count > 0) {
        s_offline_head = (s_offline_head + 1) % THP_OFFLINE_QUEUE_LEN;
        s_offline_count--;
    }
    q_unlock();
}

void thp_queue_clear(void)
{
    q_lock();
    s_offline_head = 0;
    s_offline_count = 0;
    q_unlock();
}

unsigned thp_queue_count(void)
{
    q_lock();
    unsigned n = (unsigned)s_offline_count;
    q_unlock();
    return n;
}
