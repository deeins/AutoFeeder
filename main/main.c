#include <stdio.h>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "Key.h"
#include "Motor.h"

#define MAX_SPEED_LEVEL 3
#define MOTOR_SWITCH GPIO_NUM_9

static uint8_t SpeedLevel = 0;

static uint32_t SpeedUpVal = 2000;

void Motor_cb(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    if (++SpeedLevel >= MAX_SPEED_LEVEL)
    {
        SpeedLevel = 0;
    }
    uint32_t CurSpeed = *(uint32_t*)handler_args * SpeedLevel;
    Motor_SetSpeed(CurSpeed);
    ESP_LOGI("APP", "Motor_cb set speed, SpeedLevel = %d, Speed = %d", SpeedLevel, CurSpeed);
}

ESP_EVENT_DECLARE_BASE(MOTOR_EVENTS);        // declaration of the timer events family
ESP_EVENT_DEFINE_BASE(MOTOR_EVENTS);

enum {
    MOTOR_START
};

void Key_MotorSwitchTask(void* parameter)
{
    while(1)
    {
        if (Key_GetKeyPressState(MOTOR_SWITCH))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (Key_GetKeyPressState(MOTOR_SWITCH))
            {
                ESP_LOGI("APP", "MOTOR_START");
                ESP_ERROR_CHECK(esp_event_post(MOTOR_EVENTS, MOTOR_START, NULL, 0, portMAX_DELAY));
            }
            while (Key_GetKeyPressState(MOTOR_SWITCH))
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    Key_Init(BIT(MOTOR_SWITCH));
    Motor_Init(GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12);

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(MOTOR_EVENTS, MOTOR_START, Motor_cb, &SpeedUpVal));

    // // 方式二：显式拿默认循环句柄
    // ESP_ERROR_CHECK(esp_event_handler_instance_register_with(esp_event_loop_get_default(), MOTOR_EVENTS, MOTOR_START, Motor_cb, &SpeedUpVal, NULL));

    xTaskCreate(Key_MotorSwitchTask, "Key_MotorSwitch", 2048, NULL, 1, NULL);
}