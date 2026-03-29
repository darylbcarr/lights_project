idf_component_register(
    SRCS "webserver.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server networking led ota_manager config_store event_log json freertos log esp_timer matter
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
