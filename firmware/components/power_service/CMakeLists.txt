idf_component_register(
    SRCS "power_service.cpp"
    INCLUDE_DIRS "include"
    REQUIRES board driver esp_driver_i2c esp_timer
)
