idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES Key Motor Feed TimeScheduler Encoder Debug I2C DS3231)