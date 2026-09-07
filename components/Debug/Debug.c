#include "Debug.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "Feed.h"
#include "esp_log.h"
#include "Motor.h"
#include "Encoder.h"

static const char* DB_TAG = "Debug";

static QueueHandle_t s_DebugStallQueue;

static TaskHandle_t s_DebugTaskHandle;

static DebugMode_t s_DebugMode = DB_MODE_NONE;

static uint8_t s_CanReceiveStallSrc = 1;

static float s_PreTickCircle = 0;

static void Debug_SendSingleDirStallMode(void)
{
    JamRule_t Rule = {
        .Circle = 1.0,
        .DurationMs = -1,
        .IsForwardDirStall = 1,
        .MaxTrig = 1
    };
    xQueueSend(s_DebugStallQueue, &Rule, 0);
}

static void Debug_SendDoubleDirStallMode(void)
{
    Debug_SendSingleDirStallMode();
    JamRule_t Rule = {
        .Circle = 0.95,
        .DurationMs = -1,
        .IsForwardDirStall = 0,
        .MaxTrig = 3
    };
    xQueueSend(s_DebugStallQueue, &Rule, 0);
    Rule.Circle = 0.97;
    Rule.IsForwardDirStall = 1;
    xQueueSend(s_DebugStallQueue, &Rule, 0);
}

static void Debug_SendStalledRule(void* pHandlerArgs, esp_event_base_t Base, int32_t Id, void* pEventData)
{
    s_CanReceiveStallSrc = 1;

    xQueueReset(s_DebugStallQueue);
    switch (s_DebugMode)
    {
    case DB_MODE_SINGLE_DIR_STALL:
        Debug_SendSingleDirStallMode();
        break;
    case DB_MODE_DOUBLE_DIR_STALL:
        Debug_SendDoubleDirStallMode();
        break;
    
    default:
        break;
    }
}

static void Debug_ClearStallSrc(void* pHandlerArgs, esp_event_base_t Base, int32_t Id, void* pEventData)
{
    xQueueReset(s_DebugStallQueue);
}

static void Debug_HandleStallSrc(float CurCircle)
{
    static JamRule_t Rule;
    if (s_CanReceiveStallSrc && xQueueReceive(s_DebugStallQueue, &Rule, 0) == pdTRUE)
    {
        ESP_LOGI(DB_TAG, "Receive JamRule:Circle = %f, DurationMs = %d, MaxTrig = %d.", Rule.Circle, Rule.DurationMs, Rule.MaxTrig);
        
        if (Rule.MaxTrig <= 0)
        {
            ESP_LOGI(DB_TAG, "Invalid input! The MaxTrig is %d.", Rule.MaxTrig);
            return;
        }

        s_CanReceiveStallSrc = 0;
    }

    if (!s_CanReceiveStallSrc)
    {
        // 在需要正转卡顿的时候，如果电机正在逆向旋转，那么不拦截；
        // 在需要逆转卡顿的时候，如果电机正在正向旋转，那么不拦截；
        if ((Rule.IsForwardDirStall && CurCircle <= s_PreTickCircle) || 
            (!Rule.IsForwardDirStall && CurCircle >= s_PreTickCircle))
        {
            return;
        }

        // 到达此处时，需要检测的方向与电机方向一致，所以不额外检测方向
        // 正转时，规则圈数小于等于当前圈数，那么触发卡顿；
        // 逆转时，规则圈数大于等于当前圈数，那么触发卡顿；
        if ((Rule.IsForwardDirStall && Rule.Circle <= CurCircle) ||
            (!Rule.IsForwardDirStall && Rule.Circle >= CurCircle))
        {
            s_CanReceiveStallSrc = 1;

            Rule.MaxTrig--;
            Motor_Stop();
            ESP_LOGI(DB_TAG, "Stop Motor.");

            if (Rule.MaxTrig > 0)
            {
                xQueueSend(s_DebugStallQueue, &Rule, 0);
                ESP_LOGI(DB_TAG, "Send source to stall queue in Debug_HandleStallSrc.");
            }
        }
    }
}

static void Debug_Run(void)
{
    float CurCircle = Encoder_GetOutputRotCount();
    Debug_HandleStallSrc(CurCircle);
    s_PreTickCircle = CurCircle;
}

static void Debug_RunTask(void* Parameter)
{
    while (1)
    {
        Debug_Run();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
}

static void Debug_Enter(void)
{
    ESP_LOGI(DB_TAG, "Init debug mode.");

    s_DebugStallQueue = xQueueCreate(5, sizeof(JamRule_t));

    ESP_ERROR_CHECK(esp_event_handler_register(FEED_EVENTS, FEED_START, Debug_SendStalledRule, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(FEED_EVENTS, FEED_END, Debug_ClearStallSrc, NULL));

    xTaskCreate(Debug_RunTask, "DebugTask", 4096, NULL, 1, &s_DebugTaskHandle);
}

static void Debug_Exit(void)
{
    vQueueDelete(s_DebugStallQueue);
    s_DebugStallQueue = NULL;
    ESP_ERROR_CHECK(esp_event_handler_unregister(FEED_EVENTS, FEED_START, Debug_SendStalledRule));
    ESP_ERROR_CHECK(esp_event_handler_unregister(FEED_EVENTS, FEED_END, Debug_ClearStallSrc));
    Debug_SetDebugMode(DB_MODE_NONE);

    ESP_LOGI(DB_TAG, "Exit debug mode.");
}

void Debug_SetDebugMode(DebugMode_t DbMode)
{
    s_DebugMode = DbMode;
}

void Debug_ModeRotate(void)
{
    DebugMode_t DbMode = Debug_GetDebugMode();
    if (DbMode == DB_MODE_NONE)
    {
        Debug_Enter();
    }

    Debug_SetDebugMode(DbMode + 1);

    DbMode = Debug_GetDebugMode();

    const char* ModeMsg = "Invalid mode";

    switch (DbMode)
    {
    case DB_MODE_NONE:
        ModeMsg = "DB_MODE_NONE mode, but it's impossible. There must be error";
        break;
    case DB_MODE_SINGLE_DIR_STALL:
        ModeMsg = "SINGLE_DIR_STALL mode";
        break;
    case DB_MODE_DOUBLE_DIR_STALL:
        ModeMsg = "DOUBLE_DIR_STALL mode";
        break;
    case DB_MODE_MAX:
        ModeMsg = "DB_MODE_MAX mode to exit debug mode";
        break;

    default:
        break;
    }

    ESP_LOGI("APP", "Debug mode switch, enter %s.", ModeMsg);

    if (Debug_GetDebugMode() >= DB_MODE_MAX)
    {
        Debug_Exit();
        
        vTaskDelete(s_DebugTaskHandle);
        s_DebugTaskHandle = NULL;
    }
}

DebugMode_t Debug_GetDebugMode(void)
{
    return s_DebugMode;
}
