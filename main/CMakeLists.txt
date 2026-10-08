idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        config
        storage
        led
        button
        wifi_manager
        web_server
        auth
        mdns_service
        arp_scanner
        mac_list
        settings
        time_service
        telegram
        webhook
        log_buffer
)