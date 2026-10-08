idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "include"
    REQUIRES config nvs_flash esp_littlefs
)
