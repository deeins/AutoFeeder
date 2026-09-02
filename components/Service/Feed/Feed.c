#include "Feed.h"
#include "freertos/queue.h"
#include "esp_log.h"

ESP_EVENT_DEFINE_BASE(FEED_EVENTS);

/* 红外消息结构体：红外模块未写，先占位；模块成型后移到驱动侧头文件 */
typedef struct {
    uint8_t type;   /* 0=卡粮 1=出餐口堵，占位 */
} IR_Data_t;

static QueueHandle_t Fd_Queue;
static QueueHandle_t Fd_ReqQueue;
// 红外模块还没写，暂时放这里了
static QueueHandle_t IR_Queue;

static const char *FD_TAG = "FEED";

static uint8_t s_bFdQueueOwnedImdtSource = 0;

static FdState_t s_FdState = FEED_ST_READY;

static FdData_t s_FdData;

static void Feed_RequestHandler(void* handler_args, esp_event_base_t base, int32_t id, void* event_data)
{
    xQueueSend(Fd_ReqQueue, event_data, 0);   /* 只投递：决策在喂食任务里做 */
}

void Feed_Init(void)
{
    Fd_Queue = xQueueCreate(5, sizeof(FdData_t));
    Fd_ReqQueue = xQueueCreate(5, sizeof(FdData_t));
    IR_Queue = xQueueCreate(5, sizeof(IR_Data_t));   /* 红外消息结构体，先占位 */
    assert(Fd_Queue && IR_Queue);
    esp_event_handler_register(FEED_EVENTS, FEED_REQUEST, Feed_RequestHandler, NULL);
}

void Feed_HandleRequest(FdData_t* pFdData)
{
    if (!pFdData)
    {
        return;
    }

    if (pFdData->Type == FEED_TYPE_IMMEDIATE && s_bFdQueueOwnedImdtSource)
    {
        ESP_LOGI(FD_TAG, "Reject: There is immediate feed source.");
        FdMsgData_t FdMsgData = {
            .FdData = *pFdData,
            .Msg = "Reject: There is immediate feed source."
        };
        esp_event_post(FEED_EVENTS, FEED_REJECT, &FdMsgData, sizeof(FdMsgData), 0);
        return;
    }

    if (pFdData->Type == FEED_TYPE_IMMEDIATE)
    {
        s_bFdQueueOwnedImdtSource = 1;
    }
    xQueueSend(Fd_Queue, pFdData, 0);   /* 决策通过：入作业队列（被拒的永远不会到这里） */
}

FdActRes_t Feed_ReadyHandler()
{
    if (xQueueReceive(Fd_Queue, &s_FdData, 0) == pdTRUE)
    {
        ESP_LOGI(FD_TAG, "Receive feed event source.");
        return FD_RES_JOB_TAKEN;
    }
    return FD_RES_JOB_NONE;
}

FdActRes_t Feed_RunningHandler()
{
    return FD_RES_TARGET_REACHED;
}

FdActRes_t Feed_ErrorHandler()
{
    return FD_RES_RETRY_OK;
}

FdActRes_t Feed_EndHandler()
{
    if (s_FdData.Type == FEED_TYPE_IMMEDIATE)
    {
        s_bFdQueueOwnedImdtSource = 0;
    }
    return FD_RES_DONE;
}

void Feed_IR_RunningBlockHandler()
{
    
}

void Feed_IR_ErrorBlockHandler()
{

}

FdState_t Feed_StateTransition(const FdActRes_t ActRes)
{
    static uint8_t RetryCnt = 0;
    FdState_t FdState = s_FdState;
    if (ActRes == FD_RES_IN_PROGRESS)
    {
        FdState = s_FdState;
    }
    else if (ActRes == FD_RES_JOB_NONE)
    {
        FdState = FEED_ST_READY;
    }
    else if (ActRes == FD_RES_JOB_TAKEN)
    {
        FdState = FEED_ST_RUNNING;
    }
    else if (ActRes == FD_RES_TARGET_REACHED)
    {
        FdState = FEED_ST_END;
    }
    else if (ActRes == FD_RES_NO_PULSE)
    {
        FdState = FEED_ST_ERROR;
    }
    else if (ActRes == FD_RES_RETRY_OK)
    {
        RetryCnt = 0;
        FdState = FEED_ST_RUNNING;
    }
    else if (ActRes == FD_RES_RETRY_FAIL)
    {
        RetryCnt++;
        if (RetryCnt >= FEED_ERROR_RETRY_TIMES)
        {
            RetryCnt = 0;
            FdState = FEED_ST_END;
        }
        else
        {
            FdState = FEED_ST_ERROR;
        }
    }
    else if (ActRes == FD_RES_DONE)
    {
        FdState = FEED_ST_READY;
    }
    return FdState;
}

FdActRes_t Feed_SmAct(const FdState_t FdState)
{
    FdActRes_t FdActRes = FD_RES_NONE;
    switch (FdState)
    {
    case FEED_ST_NONE:
        ESP_LOGI(FD_TAG, "None state.");
        break;
    case FEED_ST_READY:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_READY state.");
        FdActRes = Feed_ReadyHandler();
        break;
    case FEED_ST_RUNNING:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_RUNNING state.");
        FdActRes = Feed_RunningHandler();
        break;
    case FEED_ST_ERROR:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_ERROR state.");
        FdActRes = Feed_ErrorHandler();
        break;
    case FEED_ST_END:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_END state.");
        FdActRes = Feed_EndHandler();
        break;
    default:
        ESP_LOGI(FD_TAG, "ERROR: The val of FdState is invalid! FdState = %d, FdActRes = FD_RES_NONE.", FdState);
        break;
    }
    return FdActRes;
}

void Feed_OnBlockage(const IR_Data_t* pIR_Data)
{
    switch (s_FdState)
    {
    case FEED_ST_RUNNING:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_RUNNING state.");
        Feed_IR_RunningBlockHandler();
        break;
    case FEED_ST_ERROR:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_ERROR state.");
        Feed_IR_ErrorBlockHandler();
        break;
    default:
        ESP_LOGI(FD_TAG, "ERROR: Block in Unexpected FdState!");
        break;
    }
}

void Feed_Run(void)
{
    IR_Data_t IR_Data;
    if (xQueueReceive(IR_Queue, &IR_Data, 10) == pdTRUE)
    {
        /* 触发了红外守卫 */
        Feed_OnBlockage(&IR_Data);
    }

    FdData_t FdReq;
    while (xQueueReceive(Fd_ReqQueue, &FdReq, 0) == pdTRUE)
    {
        Feed_HandleRequest(&FdReq);
    }

    FdActRes_t ActRes = Feed_SmAct(s_FdState);
    s_FdState = Feed_StateTransition(ActRes);
}
