idf_component_register(SRCS "Encoder.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_pcnt)