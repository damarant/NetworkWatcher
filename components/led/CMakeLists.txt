idf_component_register(
    SRCS "led.c"
    INCLUDE_DIRS "include"
    REQUIRES config esp_driver_gpio
)