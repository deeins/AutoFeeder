#ifndef __MOTOR_H
#define __MOTOR_H
#include "driver/gpio.h"

void Motor_Init(gpio_num_t pwm,gpio_num_t ain1_pin, gpio_num_t ain2_pin);

void Motor_SetSpeed(int32_t Speed);

#endif
