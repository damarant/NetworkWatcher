idf_component_register(
    SRCS "arp_scanner.c"
    INCLUDE_DIRS "include"
    REQUIRES config wifi_manager mac_list led telegram webhook esp_netif lwip esp_timer
)