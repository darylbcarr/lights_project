idf_component_register(
    SRCS "config_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash log
)
