#include "TimeScheduler.h"
#include "DS3231.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include <stdint.h>
#include <string.h>

const char* TsTag = "TimeScheduler";

ESP_EVENT_DEFINE_BASE(TIME_EVENTS);

static QueueHandle_t s_TsRequestHandle;

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

static void TS_HandleRegisterData(TsRegisterData_t* Data)
{
    
}

static void TS_HandleCancelData(TsCancelData_t* Data)
{

}

static void TS_HandleCalibrateData(TsCalibrateData_t* Data)
{

}

static TsActRes_t TS_RunHandler(void)
{
    TsEvtItem_t EvtItem;
    if (xQueueReceive(s_TsRequestHandle, &EvtItem, 0) == pdFALSE)
    {
        return TS_RES_IN_PROGRESS;
    }

    switch (EvtItem.Id)
    {
    case TIME_REGISTER:
        TS_HandleRegisterData(&EvtItem.Data.Register);
        break;
    case TIME_CANCEL:
        TS_HandleCancelData(&EvtItem.Data.Cancel);
        break;
    case TIME_CALIBRATE:
        TS_HandleCalibrateData(&EvtItem.Data.Calibrate);
        break;
    default:
        break;
    }

    return TS_RES_IN_PROGRESS;
}

static TsActRes_t TS_PauseTimeHandler(void)
{
    if (DS3231_TimeIsValid())
    {
        return TS_RES_TIME_VALID;
    }
    return TS_RES_IN_PROGRESS;
}

static TsActRes_t TS_PauseI2CHandler(void)
{
    uint8_t StatusReg = 0;
    if (DS3231_Probe(&StatusReg))
    {
        return TS_RES_I2C_OK;
    }

    if (!DS3231_TimeIsValid())
    {
        return TS_RES_TIME_INVALID;
    }
    return TS_RES_IN_PROGRESS;
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
    case TS_ST_PAUSR_I2C:
        ActRes = TS_PauseI2CHandler();
        break;
    default:
        break;
    }
    return ActRes;
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
            const char* Msg = "Fail to create I2C connection when init. Please check the DS3231.";
            ESP_LOGI(TsTag, "%s", Msg);
            esp_event_post(TIME_EVENTS, TIME_CONNECT_FAIL, Msg, strlen(Msg), 0);
        }
    }
    else if (ActRes == TS_RES_TIME_INVALID)
    {
        const char* Msg = "Time is invalid. Please adjust the time.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_INVALID, Msg, strlen(Msg), 0);
        State = TS_ST_PAUSE_TIME;
    }
    else if (ActRes == TS_RES_TIME_VALID)
    {
        const char* Msg = "Time is valid.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_VALID, Msg, strlen(Msg), 0);
        State = TS_ST_RUN;
    }
    else if (ActRes == TS_RES_I2C_FAIL)
    {
        const char* Msg = "Fail to create I2C connection. Please check the DS3231.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_CONNECT_FAIL, Msg, strlen(Msg), 0);
        State = TS_ST_RUN;
    }
    else if (ActRes == TS_RES_I2C_OK)
    {
        const char* Msg = "I2C connection is OK.";
        ESP_LOGI(TsTag, "%s", Msg);
        esp_event_post(TIME_EVENTS, TIME_CONNECT_OK, Msg, strlen(Msg), 0);
        State = TS_ST_RUN;
    }

    return State;
}

static void TS_Run(void)
{
    TsActRes_t ActRes = TS_StateHandler(s_State);

    TS_StateTransition(ActRes);
}

static void TS_RegisterHandler(void* pHandlerArgs, esp_event_base_t Base, int32_t Id, void* pEventData)
{
    
}

void TS_Init(void)
{
    s_TsRequestHandle = xQueueCreate(TS_QUEUE_REQUEST_MAX, sizeof(TsEvtItem_t));
    esp_event_handler_register(TIME_EVENTS, TIME_REGISTER, NULL, NULL);
}

void TS_RunTask(void)
{

}