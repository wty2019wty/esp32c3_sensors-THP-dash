idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        sht40
        bmp280
        atc_ble
        esp_driver_i2c
        esp_driver_gpio
        esp_timer
        esp_system
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
        esp_http_client
        mbedtls
        lwip
        freertos
        log
        bt
)
# thp_config.h / thp_config.h.example：Wi-Fi 与 THP Token 配置（真实文件不入库）
# i2c_config.h：板级 I2C 引脚与速率（与参考工程 esp32c3_sensors 一致）
