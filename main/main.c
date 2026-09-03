#include <stdio.h>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "Key.h"
#include "Motor.h"
#include "Encoder.h"
#include "Feed.h"

#define MOTOR_SWITCH GPIO_NUM_9

FEED_SOURCE_DEFINE(FD_IMMEDIATE_TEST);

static void Key_MotorSwitchTask(void* Parameter)
{
    while(1)
    {
        if (Key_GetKeyPressState(MOTOR_SWITCH))
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            if (Key_GetKeyPressState(MOTOR_SWITCH))
            {
                ESP_LOGI("APP", "send FEED_REQUEST");
                FdData_t FdData = {
                    .Type = FEED_TYPE_IMMEDIATE,
                    .Source = FD_IMMEDIATE_TEST,
                    .Weight = 30
                };
                ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_REQUEST, &FdData, sizeof(FdData), portMAX_DELAY));
            }
            while (Key_GetKeyPressState(MOTOR_SWITCH))
            {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void FeedTask(void* Parameter)
{
    while (1)
    {
        Feed_Run();
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    Key_Init(BIT(MOTOR_SWITCH));
    Motor_Init(GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12);
    Encoder_Init(GPIO_NUM_16, GPIO_NUM_17);
    Feed_Init();

    xTaskCreate(Key_MotorSwitchTask, "Key_MotorSwitch", 2048, NULL, 1, NULL);
    xTaskCreate(FeedTask, "FeedTask", 8192, NULL, 1, NULL);
}
