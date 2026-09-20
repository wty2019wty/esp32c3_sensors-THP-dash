#include "thp_sensors.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bmp280.h"
#include "i2c_config.h"
#include "sht40.h"
#include "thp_config.h"
#include "thp_types.h"

static const char *TAG = "thp.sensor";

#define I2C_GLITCH_IGNORE_CNT 7
#define I2C_TIMEOUT_MS        100

static i2c_master_bus_handle_t s_bus;
static sht40_t s_sht;
static bmp280_t s_bmp;

static void i2c_lines_selftest(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << I2C_SDA_GPIO) | (1ULL << I2C_SCL_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "I2C 线自检配置失败");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    int sda = gpio_get_level(I2C_SDA_GPIO);
    int scl = gpio_get_level(I2C_SCL_GPIO);
    ESP_LOGI(TAG, "I2C 线自检：SDA=%d SCL=%d (1=可拉高)", sda, scl);
    if (sda == 0 || scl == 0) {
        ESP_LOGW(TAG, "I2C 线无法拉高：检查板载 LED(GPIO8)/短路/上拉电阻");
    }
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_io_num = I2C_SDA_GPIO,
        .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus_cfg, &s_bus);
}

static void sensors_retry_init_if_missing(void)
{
    if (!s_sht.present) {
        if (sht40_init(&s_sht, s_bus, I2C_SCL_SPEED_HZ) == ESP_OK) {
            ESP_LOGW(TAG, "SHT40 热修复探测成功，恢复温湿度上报");
        }
    }
    if (!s_bmp.present) {
        if (bmp280_init(&s_bmp, s_bus, I2C_SCL_SPEED_HZ) == ESP_OK) {
            ESP_LOGW(TAG, "BMP280 热修复探测成功，恢复气压上报");
        }
    }
}

esp_err_t thp_sensors_init(void)
{
    i2c_lines_selftest();
    esp_err_t err = i2c_bus_init();
    if (err != ESP_OK) {
        return err;
    }

    err = sht40_init(&s_sht, s_bus, I2C_SCL_SPEED_HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SHT40 初始化失败：暂仅上报气压（若有）；任务内会重试探测（禁止用 BMP 温度替代）");
    }
    err = bmp280_init(&s_bmp, s_bus, I2C_SCL_SPEED_HZ);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "BMP280 初始化失败：暂仅上报温湿度（若有）；任务内会重试探测");
    }

    if (!s_sht.present || !s_bmp.present) {
        ESP_LOGW(TAG, "传感器未全部就绪（SHT40=%d BMP280=%d），支持仅温湿度或仅气压上报；任务内热修复重试",
                 s_sht.present, s_bmp.present);
    }
    return ESP_OK;
}

bool thp_sensors_present_th(void)
{
    return s_sht.present;
}

bool thp_sensors_present_p(void)
{
    return s_bmp.present;
}

bool thp_sensors_sample(thp_sample_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (s_bus == NULL) {
        return false;
    }
    sensors_retry_init_if_missing();

    float sht_t = 0.0f, sht_h = 0.0f;
    float bmp_t = 0.0f, bmp_p = 0.0f, bmp_alt = 0.0f;
    int32_t t_fine = 0;

    bool sht_ok = s_sht.present && (sht40_read(&s_sht, &sht_t, &sht_h) == ESP_OK);
    bool bmp_ok = s_bmp.present && (bmp280_read(&s_bmp, &bmp_t, &bmp_p, &bmp_alt, &t_fine) == ESP_OK);

    if (!sht_ok || !bmp_ok) {
        (void)i2c_master_bus_reset(s_bus);
        if (!sht_ok) {
            sht_ok = s_sht.present && (sht40_read(&s_sht, &sht_t, &sht_h) == ESP_OK);
        }
        if (!bmp_ok) {
            bmp_ok = s_bmp.present && (bmp280_read(&s_bmp, &bmp_t, &bmp_p, &bmp_alt, &t_fine) == ESP_OK);
        }
    }

    if (sht_ok && thp_th_in_range(sht_t, sht_h)) {
        out->has_th = true;
        out->temperature = sht_t;
        out->humidity = sht_h;
    } else if (sht_ok) {
        ESP_LOGW(TAG, "温湿度超范围，本帧不带上报: T=%.2f H=%.2f",
                 (double)sht_t, (double)sht_h);
    }

    if (bmp_ok && thp_p_in_range(bmp_p)) {
        out->has_p = true;
        out->pressure = bmp_p;
    } else if (bmp_ok) {
        ESP_LOGW(TAG, "气压超范围，本帧不带上报: P=%.2f", (double)bmp_p);
    }

    out->valid = out->has_th || out->has_p;

    if (!out->valid) {
        ESP_LOGW(TAG, "采样无有效字段: SHT40=%s BMP280=%s present(th=%d p=%d)",
                 sht_ok ? "OK" : "FAIL", bmp_ok ? "OK" : "FAIL",
                 s_sht.present, s_bmp.present);
        return false;
    }

    if (!sht_ok || !bmp_ok) {
        ESP_LOGW(TAG, "部分采样: SHT40=%s BMP280=%s → 上报%s%s",
                 sht_ok ? "OK" : "FAIL", bmp_ok ? "OK" : "FAIL",
                 out->has_th ? " 温湿度" : "", out->has_p ? " 气压" : "");
    }
    return true;
}
