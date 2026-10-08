idf_component_register(
    SRCS "webhook.c"
    INCLUDE_DIRS "include"
    REQUIRES
        config
        settings
        mac_list
        oui_lookup
        esp_http_client
        esp-tls
        mbedtls
        esp_netif
        esp_timer
)