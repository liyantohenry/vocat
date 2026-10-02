idf_component_register(
    SRCS "app_main.c" "wifi_setup.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES app_ui dns_server esp_wifi esp_netif esp_event esp_http_server nvs_flash esp_timer lwip
)
