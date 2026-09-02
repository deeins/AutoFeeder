idf_component_register(SRCS "Motor.c"
                    PRIV_REQUIRES esp_event
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_ledc)