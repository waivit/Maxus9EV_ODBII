idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer esp_wifi esp_netif esp_event esp_http_server
             nvs_flash app_update esp_partition
)
