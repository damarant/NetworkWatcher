idf_component_register(
    SRCS "time_service.c"
    INCLUDE_DIRS "include"
    REQUIRES config mac_list esp_timer lwip
)