idf_component_register(
    SRCS "display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c k0i05__esp_ssd1306
    PRIV_REQUIRES driver
)
