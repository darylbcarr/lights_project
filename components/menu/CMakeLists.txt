idf_component_register(
    SRCS "menu.cpp"
    INCLUDE_DIRS "include"
    REQUIRES
        display
        networking
        led
        ota_manager
        config_store
        esp_timer
        freertos
        log
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
