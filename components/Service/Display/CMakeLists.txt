idf_component_register(SRCS "Display.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_event Feed TimeScheduler OLED)
