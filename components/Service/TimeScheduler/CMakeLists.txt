idf_component_register(SRCS "TS_Heap.c" "TimeScheduler.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_event esp_timer DS3231)