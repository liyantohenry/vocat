idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd
    PRIV_REQUIRES esp_http_client json mbedtls esp_timer esp_netif esp_event nvs_flash hal esp_driver_gpio esp_driver_i2c
)
