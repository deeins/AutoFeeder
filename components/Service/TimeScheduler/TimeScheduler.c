#include "TimeScheduler.h"
#include "DS3231.h"
#include "TS_Heap.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include <stdint.h>
#include <string.h>
#include <time.h>

const char* TsTag = "TimeScheduler";

ESP_EVENT_DEFINE_BASE(TIME_EVENTS);

static QueueHandle_t s_ReqQueueHandle;
// Timer回灌通知，告知运行态暂停处理其他请求，优先处理Timer收尾
static QueueHandle_t s_TimerQueueHandle;
static QueueSetHandle_t s_QueueSetHandle;

static esp_timer_handle_t s_Alarm;

static TsState_t s_State = TS_ST_INIT;

static TsActRes_t s_LastRes = TS_RES_NONE;

static TsActRes_t TS_InitHandler(void)
{
    esp_err_t err_code = DS3231_Init();
    if (err_code != ESP_OK)
    {
        return TS_RES_INIT_I2C_FAIL;
    }

    if (!DS3231_TimeIsValid())
    {
        return TS_RES_TIME_INVALID;
    }
    return TS_RES_INIT_DONE;
}

static void TS_ExpiredPolicy(TsRegisterData_t* Data, uint64_t Epoch)
{
    if (Data == NULL)
    {
        return;
    }
    esp_event_post(Data->TargetBase, Data->TargetId, Data->TargetPayload, Data->PayloadLen, 0);

    // 无论是否续约，都要弹出旧数据，所以可以在TS_HandleExpiredData里统一弹出
    if (Data->Periodic)
    {
        Data->DueLocalEpoch += Data->PeriodSec;
        TS_HEAP_Push(Data);
    }
}

static void TS_MissedPolicy(TsRegisterData_t* Data, uint64_t Epoch)
{
    if (Data == NULL || !Data->Periodic || Data->PeriodSec == 0)
    {
        return;
    }

    while (Data->DueLocalEpoch <= Epoch)
    {
        Data->DueLocalEpoch += Data->PeriodSec;
    }
    TS_HEAP_Push(Data);
}

static void TS_HandleExpiredData(uint64_t Epoch, void Policy(TsRegisterData_t* Data, uint64_t Now))
{
    while (!TS_HEAP_Empty())
    {
        TsRegisterData_t Data;
        TS_HEAP_Top(&Data);
        // 最小时间戳都还没过期就跳出
        if (Data.DueLocalEpoch > Epoch)
        {
            break;
        }
        
        TS_HEAP_Pop();
        if (Policy != NULL)
        {
            Policy(&Data, Epoch);
        }
    }
}

static void TS_Timer_cb(void* Parameter)
{
    xQueueSend(s_TimerQueueHandle, NULL, 0);
}

static void TS_PostReject(esp_event_base_t Source, uint32_t Handle, const char* Msg)
{
    TsRejectData_t RejData = {
        .Handle = Handle,
        .Source = Source,
        .Msg = Msg
    };
    esp_event_post(TIME_EVENTS, TIME_REJECT, &RejData, sizeof(TsRejectData_t), 0);
}

static void TS_RejectPost(TsRegisterData_t* Data, const char* Msg)
{
    if (!Data)
    {
        ESP_LOGI(TsTag, "TS_RejectPost: Data is Null.");
        return;
    }
    ESP_LOGI(TsTag, "%s", Msg);
    TS_PostReject(Data->Head.Source, Data->Handle, Msg);
}

static void TS_RearmTimer(uint64_t cur_epoch)
{
    TsRegisterData_t CurRoot;
    if (!TS_HEAP_Top(&CurRoot))
    {
        esp_timer_stop(s_Alarm);
        return;
    }

    uint64_t DiffSec = (CurRoot.DueLocalEpoch > cur_epoch) ? (CurRoot.DueLocalEpoch - cur_epoch) : 0;
    uint64_t TimeoutUs = DiffSec * 1000000ULL;
    if (esp_timer_is_active(s_Alarm)) 
    {
        esp_timer_restart(s_Alarm, TimeoutUs);
    } 
    else
    {
        esp_timer_start_once(s_Alarm, TimeoutUs);
    }
}

static void TS_HandleRegisterData(TsRegisterData_t* Data, struct tm *cur_tm)
{
    if (Data == NULL)
    {
        const char* Msg = "Register data is NULL.";
        ESP_LOGI(TsTag, "%s", Msg);
        return;
    }

    if (Data->Periodic && Data->PeriodSec == 0)
    {
        TS_RejectPost(Data, "RegisterData is periodic but PeriodSec is zero.");
        return;
    }

    if (Data->PayloadLen > TS_PAYLOAD_MAX)
    {
        TS_RejectPost(Data, "PayloadLen is larger than TS_PAYLOAD_MAX.");
        return;
    }

    // 添加前先验证是否过期
    time_t cur_epoch = mktime(cur_tm);
    if (cur_epoch > Data->DueLocalEpoch)
    {
        TS_RejectPost(Data, "Epoch of register data is expired.");
        return;
    }

    // 避免出现相同句柄的元素
    uint32_t Handles = Data->Handle;
    TS_HEAP_RemoveByHandles(Data->Head.Source, &Handles, 1);

    if (!TS_HEAP_Push(Data))
    {
        TS_RejectPost(Data, "Fail to push data to heap.");
        return;
    }

    TS_RearmTimer(cur_epoch);
}

static void TS_HandleCancelData(TsCancelData_t* Data, uint64_t cur_epoch, bool Rearm)
{
    if (Data == NULL || Data->Count > TS_CANCEL_MAX)
    {
        const char* Msg = "Cancel data is invalid.";
        ESP_LOGI(TsTag, "%s", Msg);
        if (Data != NULL)
        {
            TS_PostReject(Data->Head.Source, 0, Msg);
        }
        return;
    }

    if (Data->Count == 0)
    {
        TS_HEAP_Clear();
    }
    else if (!TS_HEAP_RemoveByHandles(Data->Head.Source, Data->Handles, Data->Count))
    {
        TS_PostReject(Data->Head.Source, 0, "No handle is removed.");
        return;
    }

    ESP_LOGI(TsTag, "Success to cancel data from %s", Data->Head.Source);
    if (Rearm)
    {
        TS_RearmTimer(cur_epoch);
    }
}

static esp_err_t TS_HandleCalibrateData(TsCalibrateData_t* Data)
{
    if (Data == NULL)
    {
        const char* Msg = "Calibrate data is NULL.";
        ESP_LOGI(TsTag, "%s", Msg);
        return ESP_ERR_INVALID_ARG;
    }

    struct tm temp_tm;
    time_t temp_time = Data->Epoch;
    gmtime_r(&temp_time, &temp_tm);
    return DS3231_SetTime(&temp_tm);
}

static TsActRes_t TS_RunHandler(void)
{
    if (xQueueReceive(s_TimerQueueHandle, NULL, 0) == pdTRUE && !TS_HEAP_Empty())
    {
        struct tm temp_tm;
        if (DS3231_GetTime(&temp_tm) != ESP_OK)
        {
            // 恢复会自动处理过期信息，此处不需要额外处理，虽然是直接删
            return TS_RES_I2C_FAIL;
        }
        uint64_t cur_epoch = mktime(&temp_tm);
        TS_HandleExpiredData(cur_epoch, TS_ExpiredPolicy);
        TS_RearmTimer(cur_epoch);
    }

    TsEvtItem_t EvtItem;
    if (xQueueReceive(s_ReqQueueHandle, &EvtItem, 0) == pdFALSE)
    {
        return TS_RES_IN_PROGRESS;
    }

    // 均为一次性动作，放此处与放状态转移等价，因为队列资源会消耗
    // 放状态转移反而麻烦，因为要传递请求数据
    struct tm cur_tm;
    switch (EvtItem.Id)
    {
    case TIME_REGISTER:
        if (DS3231_GetTime(&cur_tm) != ESP_OK)
        {
            return TS_RES_I2C_FAIL;
        }
        TS_HandleRegisterData(&EvtItem.Data.Register, &cur_tm);
        break;
    case TIME_CANCEL:
        if (DS3231_GetTime(&cur_tm) != ESP_OK)
        {
            return TS_RES_I2C_FAIL;
        }
        TS_HandleCancelData(&EvtItem.Data.Cancel, mktime(&cur_tm), true);
        break;
    case TIME_CALIBRATE:
        if (TS_HandleCalibrateData(&EvtItem.Data.Calibrate) != ESP_OK)
        {
            return TS_RES_I2C_FAIL;
        }
        else 
        {
            uint64_t cal_epoch = EvtItem.Data.Calibrate.Epoch;
            TS_HandleExpiredData(cal_epoch, TS_MissedPolicy);
            TS_RearmTimer(cal_epoch);
            ESP_LOGI(TsTag, "Success to calibrate time.");
        }
        break;
    default:
        break;
    }

    return TS_RES_IN_PROGRESS;
}

static TsActRes_t TS_PauseTimeHandler(void)
{
    TsEvtItem_t EvtItem;
    if (xQueueReceive(s_ReqQueueHandle, &EvtItem, 0) == pdTRUE)
    {
        switch (EvtItem.Id)
        {
        case TIME_CALIBRATE:
            if (TS_HandleCalibrateData(&EvtItem.Data.Calibrate) != ESP_OK)
            {
                return TS_RES_I2C_FAIL;
            }
            ESP_LOGI(TsTag, "Success to calibrate time.");
            return TS_RES_TIME_VALID;
        case TIME_CANCEL:
            TS_HandleCancelData(&EvtItem.Data.Cancel, 0, false);
            break;
        case TIME_REGISTER:
            TS_RejectPost(&EvtItem.Data.Register, "Time is invalid, register is rejected.");
            break;
        default:
            break;
        }
    }

    if (DS3231_TimeIsValid())
    {
        return TS_RES_TIME_VALID;
    }
    return TS_RES_IN_PROGRESS;
}

static TsActRes_t TS_PauseI2CHandler(void)
{
    uint8_t StatusReg = 0;
    if (DS3231_Probe(&StatusReg) != ESP_OK)
    {
        return TS_RES_IN_PROGRESS;
    }

    if (!DS3231_TimeIsValid())
    {
        return TS_RES_TIME_INVALID;
    }
    return TS_RES_I2C_OK;
}

static TsActRes_t TS_StateHandler(const TsState_t State)
{
    TsActRes_t ActRes = TS_RES_NONE;
    switch (State)
    {
    case TS_ST_INIT:
        ActRes = TS_InitHandler();
        break;
    case TS_ST_RUN:
        ActRes = TS_RunHandler();
        break;
    case TS_ST_PAUSE_TIME:
        ActRes = TS_PauseTimeHandler();
        break;
    case TS_ST_PAUSE_I2C:
        ActRes = TS_PauseI2CHandler();
        break;
    default:
        break;
    }
    return ActRes;
}

static void TS_HandlePauseExpiredData(TsState_t* State, const char* ErrorActResName)
{
    struct tm temp_tm;
    if (DS3231_GetTime(&temp_tm) == ESP_OK)
    {
        uint64_t cur_epoch = mktime(&temp_tm);
        TS_HandleExpiredData(cur_epoch, TS_MissedPolicy);
        TS_RearmTimer(cur_epoch);
        *State = TS_ST_RUN;
    }
    else
    {
        char* Msg = "Fail to create I2C connection when running state transition.";
        ESP_LOGI(TsTag, "%s",Msg);
        ESP_LOGI(TsTag, "ActRes = %s", ErrorActResName);
        esp_event_post(TIME_EVENTS, TIME_CONNECT_FAIL, Msg, strlen(Msg), 0);
        *State = TS_ST_PAUSE_I2C;
    }
}

static TsState_t TS_StateTransition(TsActRes_t ActRes)
{
    TsState_t State = s_State;

    if (ActRes == TS_RES_INIT_DONE)
    {
        State = TS_ST_RUN;
    }
    else if (ActRes == TS_RES_INIT_I2C_FAIL)
    {
        if (s_LastRes != TS_RES_INIT_I2C_FAIL)
        {
            char* Msg = "Fail to create I2C connection when init.";
            ESP_LOGI(TsTag, "%s", Msg);
            esp_event_post(TIME_EVENTS, TIME_CONNECT_FAIL, Msg, strlen(Msg), 0);
        }
    }
    else if (ActRes == TS_RES_TIME_INVALID)
    {
        char* Msg = "Time is invalid. Please adjust the time.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_INVALID, Msg, strlen(Msg), 0);
        State = TS_ST_PAUSE_TIME;
    }
    else if (ActRes == TS_RES_TIME_VALID)
    {
        char* Msg = "Time is valid.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_VALID, Msg, strlen(Msg), 0);
        if (s_State != TS_ST_INIT)
        {
            TS_HandlePauseExpiredData(&State, "TS_RES_TIME_VALID");
        }
        else 
        {
            State = TS_ST_RUN;
        }
    }
    else if (ActRes == TS_RES_I2C_FAIL)
    {
        char* Msg = "Fail to create I2C connection.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_CONNECT_FAIL, Msg, strlen(Msg), 0);
        State = TS_ST_PAUSE_I2C;
    }
    else if (ActRes == TS_RES_I2C_OK)
    {
        char* Msg = "I2C connection is OK.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_CONNECT_OK, Msg, strlen(Msg), 0);
        TS_HandlePauseExpiredData(&State, "TS_RES_I2C_OK");
    }

    if (State != s_State && (State == TS_ST_PAUSE_TIME || State == TS_ST_PAUSE_I2C))
    {
        esp_timer_stop(s_Alarm);
    }

    return State;
}

static void TS_Run(void)
{
    TsActRes_t ActRes = TS_StateHandler(s_State);

    s_State = TS_StateTransition(ActRes);

    s_LastRes = ActRes;
}

static void TS_SendReq(void* pHandlerArgs, esp_event_base_t Base, int32_t Id, void* pEventData)
{
    if (pEventData == NULL)
    {
        return;
    }

    TsEvtItem_t Item = {
        .Id = Id,
    };
    switch (Id)
    {
    case TIME_REGISTER:
        memcpy(&Item.Data.Register, pEventData, sizeof(TsRegisterData_t));
        break;
    case TIME_CANCEL:
        memcpy(&Item.Data.Cancel, pEventData, sizeof(TsCancelData_t));
        break;
    case TIME_CALIBRATE:
        memcpy(&Item.Data.Calibrate, pEventData, sizeof(TsCalibrateData_t));
        break;
    default:
        return;
    }
    xQueueSend(s_ReqQueueHandle, &Item, 0);
}

void TS_Init(void)
{
    s_ReqQueueHandle = xQueueCreate(TS_QUEUE_REQUEST_MAX, sizeof(TsEvtItem_t));
    s_TimerQueueHandle = xQueueCreate(TS_QUEUE_TIMER_MAX, 0);
    s_QueueSetHandle = xQueueCreateSet(TS_QUEUE_REQUEST_MAX + TS_QUEUE_TIMER_MAX);
    xQueueAddToSet(s_ReqQueueHandle, s_QueueSetHandle);
    xQueueAddToSet(s_TimerQueueHandle, s_QueueSetHandle);

    esp_timer_create_args_t create_args = {
        .callback        = TS_Timer_cb,
        .arg             = NULL,             /* 单例不需要携带参数 */
        .dispatch_method = ESP_TIMER_TASK,   /* 默认；回调在 esp_timer 任务上下文 */
        .name            = "Ts_Timer",
    };
    esp_timer_create(&create_args, &s_Alarm);

    esp_event_handler_register(TIME_EVENTS, TIME_REGISTER, TS_SendReq, NULL);
    esp_event_handler_register(TIME_EVENTS, TIME_CANCEL, TS_SendReq, NULL);
    esp_event_handler_register(TIME_EVENTS, TIME_CALIBRATE, TS_SendReq, NULL);

    xTaskCreate(TS_RunTask, "TS_RunTask", 4096, NULL, 1, NULL);
}

void TS_RunTask(void* Parameter)
{
    while (1)
    {
        if (s_State == TS_ST_RUN)
        {
            xQueueSelectFromSet(s_QueueSetHandle, portMAX_DELAY);
        }
        else
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        TS_Run();
    }
}