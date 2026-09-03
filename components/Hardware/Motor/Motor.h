#ifndef __MOTOR_H
#define __MOTOR_H
#include "driver/gpio.h"

void Motor_Init(gpio_num_t PwmPin, gpio_num_t Ain1Pin, gpio_num_t Ain2Pin);

void Motor_SetSpeed(int32_t Speed);

void Motor_Run(void);

void Motor_Recover_RunInverse(void);

void Motor_Stop(void);

#endif
