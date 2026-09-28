idf_component_register(
    SRCS "atc_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        mbedtls
        esp_driver_gpio
        esp_timer
        freertos
        log
        nvs_flash
)
