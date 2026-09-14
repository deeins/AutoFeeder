idf_component_register(SRCS "OLED.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio I2C)