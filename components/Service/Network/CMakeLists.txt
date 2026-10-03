idf_component_register(SRCS "NetworkInit.c" "Wifi.c"
                    PRIV_REQUIRES esp_wifi nvs_flash
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer
                    )