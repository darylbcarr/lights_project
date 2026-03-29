idf_component_register(
    SRCS
        "main.cpp"
        "console_commands.cpp"
    INCLUDE_DIRS
        "."
    REQUIRES
        event_log
        networking
        display
        menu
        led
        matter
        webserver
        config_store
        driver
        freertos
        log
        nvs_flash
        esp_wifi
        ota_manager
        app_update
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
