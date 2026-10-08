idf_component_register(
    SRCS "telegram.c"
    INCLUDE_DIRS "include"
    REQUIRES
        config
        settings
        esp_http_client
        esp-tls
        mbedtls
        esp_netif
        esp_timer
)