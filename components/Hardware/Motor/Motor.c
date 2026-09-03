#include "Motor.h"
#include "driver/ledc.h"

#define MOTOR_TIMER_CHANNEL LEDC_CHANNEL_0
#define MOTOR_TIMER LEDC_TIMER_0

#define MOTOR_DEFAULT_SPEED 2048

static gpio_num_t s_PwmPin = 0, s_Ain1Pin = 0, s_Ain2Pin = 0;

void Motor_Init(gpio_num_t PwmPin, gpio_num_t Ain1Pin, gpio_num_t Ain2Pin)
{
    s_PwmPin = PwmPin;
    s_Ain1Pin = Ain1Pin;
    s_Ain2Pin = Ain2Pin;
    const gpio_config_t Cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = BIT(Ain1Pin) | BIT(Ain2Pin),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&Cfg);
    gpio_set_level(Ain1Pin, 0);
    gpio_set_level(Ain2Pin, 0);

    // 1. 配置定时器（决定频率和分辨率）
    ledc_timer_config_t TimerCfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = MOTOR_TIMER,
        .duty_resolution = LEDC_TIMER_13_BIT,   // 0~8191
        .freq_hz         = 5000,                // 5kHz
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&TimerCfg);

    // 2. 配置通道（这里指定 GPIO，自动完成复用）
    ledc_channel_config_t ChannelCfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = MOTOR_TIMER_CHANNEL,
        .timer_sel  = MOTOR_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PwmPin,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ChannelCfg);

    // 3. 设占空比 / 更新
    ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL, 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL);
}

void Motor_SetSpeed(int32_t Speed)
{
    if (Speed >= 0)
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL, (uint32_t)Speed);
        gpio_set_level(s_Ain1Pin, 1);
        gpio_set_level(s_Ain2Pin, 0);
    }
    else
    {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL, (uint32_t)(-Speed));
        gpio_set_level(s_Ain1Pin, 0);
        gpio_set_level(s_Ain2Pin, 1);
    }
    
    ledc_update_duty(LEDC_LOW_SPEED_MODE, MOTOR_TIMER_CHANNEL);
}

void Motor_Run(void)
{
    Motor_SetSpeed(MOTOR_DEFAULT_SPEED);
}

void Motor_Stop(void)
{
    Motor_SetSpeed(0);
}
