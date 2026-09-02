#include "Motor.h"
#include "driver/ledc.h"

#define MOTOR_TIMER_CHANNEL LEDC_CHANNEL_0
#define MOTOR_TIMER LEDC_TIMER_0

static gpio_num_t pwm_pin = 0, ain1_pin = 0, ain2_pin = 0;

void Motor_Init(gpio_num_t pwm,gpio_num_t ain1, gpio_num_t ain2)
{
    pwm_pin = pwm;
    ain1_pin = ain1;
    ain2_pin = ain2;
    const gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT(ain1) | BIT(ain2),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&cfg);
    gpio_set_level(ain1, 0);
    gpio_set_level(ain2, 0);

    // 1. 配置定时器（决定频率和分辨率）
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = MOTOR_TIMER,
        .duty_resolution = LEDC_TIMER_13_BIT,   // 0~8191
        .freq_hz         = 5000,                // 5kHz
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    // 2. 配置通道（这里指定 GPIO，自动完成复用）
    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = MOTOR_TIMER_CHANNEL,
        .timer_sel  = MOTOR_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = pwm_pin,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    // 3. 设占空比 / 更新
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL);
}

void Motor_SetSpeed(int32_t Speed)
{
    if (Speed >= 0)
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL, (uint32_t)Speed);
        gpio_set_level(ain1_pin, 1);
        gpio_set_level(ain2_pin, 0);
    }
    else
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL, (uint32_t)(-Speed));
        gpio_set_level(ain1_pin, 0);
        gpio_set_level(ain2_pin, 1);
    }
    
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL);
}
