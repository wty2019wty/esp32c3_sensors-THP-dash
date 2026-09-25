#include "thp_wifi.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "thp_config.h"

static const char *TAG = "thp.wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

/* 断线后无限重连（传感器场景）；计数仅用于日志节奏 */
#define WIFI_RETRY_LOG_EVERY 8

static EventGroupHandle_t s_wifi_events;
static int s_wifi_retry;
static bool s_wifi_started;
static bool s_wifi_inited;

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_retry++;
        if (s_wifi_retry == 1 || (s_wifi_retry % WIFI_RETRY_LOG_EVERY) == 0) {
            ESP_LOGW(TAG, "Wi-Fi 断开，持续重连 #%d", s_wifi_retry);
        }
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Wi-Fi 已连接，IP=" IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupClearBits(s_wifi_events, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_apply_tx_power(void)
{
    /* Super Mini 天线差：单位 0.25 dBm，夹到 8~20 dBm */
    int8_t tx_dbm = (int8_t)THP_WIFI_STA_TX_POWER_DBM;
    if (tx_dbm < 8) {
        tx_dbm = 8;
    } else if (tx_dbm > 20) {
        tx_dbm = 20;
    }
    esp_err_t pwr_err = esp_wifi_set_max_tx_power((int8_t)(tx_dbm * 4));
    if (pwr_err != ESP_OK) {
        ESP_LOGW(TAG, "set max TX power (%d dBm) failed: %s", (int)tx_dbm, esp_err_to_name(pwr_err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi STA max TX power = %d dBm", (int)tx_dbm);
    }
}

static void wifi_apply_ps(void)
{
    /* modem sleep：无 TX/RX 时射频休眠；配合 light sleep 降空闲功耗 */
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_ps(MIN_MODEM) failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi PS = MIN_MODEM");
    }
}

static bool wait_connected(uint32_t timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

esp_err_t thp_wifi_init_sta(void)
{
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, THP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, THP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_started = true;
    s_wifi_inited = true;

    wifi_apply_tx_power();
    wifi_apply_ps();

    ESP_LOGI(TAG, "Wi-Fi STA 启动，SSID=%s", THP_WIFI_SSID);

    if (wait_connected(THP_WIFI_CONNECT_TIMEOUT_MS)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Wi-Fi 首次连接超时（%ums），事件回调仍会继续重连",
             (unsigned)THP_WIFI_CONNECT_TIMEOUT_MS);
    xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    return ESP_FAIL;
}

esp_err_t thp_wifi_radio_on(uint32_t timeout_ms)
{
    if (!s_wifi_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_wifi_started) {
        esp_err_t err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(err));
            return err;
        }
        s_wifi_started = true;
        wifi_apply_ps();
        wifi_apply_tx_power();
        ESP_LOGI(TAG, "Wi-Fi 射频开启，等待连接…");
    }
    if (wait_connected(timeout_ms)) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Wi-Fi 射频开启后 %ums 内未连上", (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

void thp_wifi_radio_off(void)
{
    if (!s_wifi_inited || !s_wifi_started) {
        return;
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
        return;
    }
    s_wifi_started = false;
    ESP_LOGI(TAG, "Wi-Fi 射频已关（进入空闲/light sleep）");
}

bool thp_wifi_is_connected(void)
{
    if (s_wifi_events == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) != 0;
}

bool thp_wifi_get_rssi(int *rssi)
{
    wifi_ap_record_t ap;
    if (rssi == NULL) {
        return false;
    }
    if (!s_wifi_started) {
        return false;
    }
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    *rssi = ap.rssi;
    return true;
}
