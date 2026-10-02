idf_component_register(
    SRC_DIRS "src"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_driver_touch_sens esp_lcd console
                  esp_http_client json mbedtls esp_timer esp_netif nvs_flash
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wakeup.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/finish_reminder.mp3"
    EMBED_FILES "${CMAKE_CURRENT_LIST_DIR}/../assets/audio/wakeup_end.mp3"
)
