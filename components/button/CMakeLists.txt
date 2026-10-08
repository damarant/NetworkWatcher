idf_component_register(
    SRCS "button.c"
    INCLUDE_DIRS "include"
    REQUIRES config esp_driver_gpio esp_timer
)
