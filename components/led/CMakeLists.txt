idf_component_register(
    SRCS "led_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES espressif__led_strip driver freertos log esp_timer
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
