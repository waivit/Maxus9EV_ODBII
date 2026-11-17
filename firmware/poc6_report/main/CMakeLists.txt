idf_component_register(
    SRCS "main.c" "uds.c" "safety.c"
    INCLUDE_DIRS "."
    REQUIRES bt nvs_flash esp_wifi esp_http_server esp_netif esp_event esp_driver_gpio
    EMBED_TXTFILES "report.js"
)
