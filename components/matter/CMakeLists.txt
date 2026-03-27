idf_component_register(
    SRCS "matter_bridge.cpp"
    INCLUDE_DIRS "include"
    REQUIRES led esp_matter nvs_flash log freertos efuse esp_wifi
)

# Suppress warnings that come from the CHIP/Matter SDK headers
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-deprecated-declarations
    -Wno-type-limits
    -Wno-unused-variable
    -Wno-shadow
)
