idf_component_register(
    SRCS "event_log.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash freertos log esp_timer json
)
