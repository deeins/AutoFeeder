#include <stdio.h>
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "Key.h"
#include "Motor.h"
#include "Feed.h"
#include "Encoder.h"

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

/*
 * [测试专用·方案② 注入模拟卡粮] 本任务在喂食进入 RUNNING 后：
 *   1) 冻结编码器计数（假卡粮）→ 观察状态机进 ERROR（异常逻辑由你实现）；
 *   2) 800ms 后恢复真实计数（假松开）→ 观察自愈/恢复回 RUNNING → 跑完到 END。
 * 跑完即自删；正式版删除本任务与 Encoder_TestSetPulse 调用即可。
 */
static void JamTestTask(void* Parameter)
{
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI("JAMTEST", "waiting for feed RUNNING ...");
    while (Feed_GetState() != FEED_ST_RUNNING)
    {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (Feed_GetState() == FEED_ST_READY)
        {
            continue;   /* 未到喂食：继续等 */
        }
    }

    ESP_LOGI("JAMTEST", "simulate jam: freeze encoder pulses");
    Encoder_TestSetPulse(Encoder_GetCount());   /* 冻结为当前值 => 计数不再变化 */

    vTaskDelay(pdMS_TO_TICKS(800));             /* 数个 tick 触发 NO_PULSE → ERROR */

    ESP_LOGI("JAMTEST", "simulate recover: resume real encoder");
    Encoder_TestSetPulse(-1);                   /* 恢复真实计数 */

    vTaskDelete(NULL);
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
    xTaskCreate(JamTestTask, "JamTest", 3072, NULL, 1, NULL);   /* [测试] 模拟卡粮，跑完自删 */
}
