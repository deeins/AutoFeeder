idf_component_register(SRCS "DS3231.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio I2C)