idf_component_register(SRCS "TimeScheduler.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_event esp_timer DS3231)