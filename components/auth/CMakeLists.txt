idf_component_register(
    SRCS "auth.c"
    INCLUDE_DIRS "include"
    REQUIRES config storage mbedtls esp_timer
)