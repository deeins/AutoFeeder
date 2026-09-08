#include <stdio.h>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "Key.h"
#include "Motor.h"
#include "Feed.h"
#include "Encoder.h"
#include "Debug.h"
#include "I2C.h"
#include "DS3231.h"
#include "soc/gpio_num.h"

#define MOTOR_SWITCH GPIO_NUM_9
#define DEBUG_MODE_SWITCH GPIO_NUM_8
#define DS3231_SDA GPIO_NUM_21
#define DS3231_SCL GPIO_NUM_47

FEED_SOURCE_DEFINE(FD_IMMEDIATE_TEST);

// 暂时放在main里，严格来讲这是外部对喂食模块的请求，
// 后面可以出一个按键服务模块，以及和手机应用对接的网络模块，这两个模块发起喂食请求
static void Feed_SendImdtRequest(void)
{
    ESP_LOGI("APP", "send FEED_REQUEST");
    FdData_t FdData = {
        .Type = FEED_TYPE_IMMEDIATE,
        .Source = FD_IMMEDIATE_TEST,
        .Weight = 30
    };
    ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_REQUEST, &FdData, sizeof(FdData), portMAX_DELAY));
}

static void Key_MotorSwitchTask(void* Parameter)
{
    while(1)
    {
        Key_SingleClickCheck(MOTOR_SWITCH, Feed_SendImdtRequest);
    }
}

static void Key_DebugModeSwitchTask(void* Parameter)
{
    while(1)
    {
        Key_SingleClickCheck(DEBUG_MODE_SWITCH, Debug_ModeRotate);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    I2C_BusInit(DS3231_SCL, DS3231_SDA);

    Key_Init(BIT(MOTOR_SWITCH) | BIT(DEBUG_MODE_SWITCH));
    Motor_Init(GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12);
    Encoder_Init(GPIO_NUM_16, GPIO_NUM_17);
    DS3231_Init();

    Feed_Init();

    xTaskCreate(Key_MotorSwitchTask, "Key_MotorSwitch", 2048, NULL, 1, NULL);
    xTaskCreate(Key_DebugModeSwitchTask, "Key_DebugModeSwitchTask", 2048, NULL, 1, NULL);
}
