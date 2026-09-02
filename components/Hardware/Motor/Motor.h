#ifndef __MOTOR_H
#define __MOTOR_H
#include "driver/gpio.h"
#include "esp_event.h"

ESP_EVENT_DECLARE_BASE(MOTOR_EVENTS); // 声明放头文件，定义在.c里，如果在.h里定义，会因为其他文件包含了本文件内容，导致多重定义

enum {
    MOTOR_START
};

void Motor_Init(gpio_num_t pwm,gpio_num_t ain1_pin, gpio_num_t ain2_pin);

void Motor_SetSpeed(int32_t Speed);

#endif
