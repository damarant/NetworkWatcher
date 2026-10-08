idf_component_register(
    SRCS "web_server.c"
    INCLUDE_DIRS "include"
    REQUIRES config auth wifi_manager arp_scanner mac_list settings led storage telegram webhook oui_lookup log_buffer esp_http_server
)