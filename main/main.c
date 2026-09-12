#include "esp_event_base.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "Key.h"
#include "Motor.h"
#include "Feed.h"
#include "TimeScheduler.h"
#include "TS_Heap.h"
#include "Encoder.h"
#include "Debug.h"
#include "I2C.h"
#include "soc/gpio_num.h"
#include <stdint.h>

#define MOTOR_SWITCH GPIO_NUM_9
#define DEBUG_MODE_SWITCH GPIO_NUM_8
#define DS3231_SDA GPIO_NUM_21
#define DS3231_SCL GPIO_NUM_47

ESP_EVENT_DEFINE_BASE(FD_IMMEDIATE_TEST);

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

ESP_EVENT_DEFINE_BASE(TS_TEST);
enum {
    TS_TEST_ID,
};

/* 堆自测入口（测试脚手架）：乱序有序弹出/同刻/删头中尾/重复句柄/异源不删/堆满/全清 */
static void TS_Heap_SelfTest(void)
{
    int FailCnt = 0;
    TsRegisterData_t E = {
        .Head.Source = TS_TEST,
    };

    /* 1) 乱序插入 → 弹出单调不减 */
    const uint64_t Dues[5] = {50, 10, 30, 20, 40};
    for (uint32_t i = 0; i < 5; i++)
    {
        E.DueLocalEpoch = Dues[i];
        E.Handle = i;
        if (!TS_HEAP_Push(&E)) FailCnt++;
    }
    uint64_t Last = 0;
    uint32_t PopCnt = 0;
    while (!TS_HEAP_Empty())
    {
        TsRegisterData_t T;
        if (!TS_HEAP_Top(&T)) { FailCnt++; break; }
        if (T.DueLocalEpoch < Last) FailCnt++;
        Last = T.DueLocalEpoch;
        TS_HEAP_Pop();
        PopCnt++;
    }
    if (PopCnt != 5) FailCnt++;

    /* 2) 同刻 3 条：弹出数量一致（相对序不保证） */
    for (uint32_t i = 0; i < 3; i++)
    {
        E.DueLocalEpoch = 100;
        E.Handle = 100 + i;
        if (!TS_HEAP_Push(&E)) FailCnt++;
    }
    PopCnt = 0;
    while (!TS_HEAP_Empty())
    {
        TsRegisterData_t T;
        if (!TS_HEAP_Top(&T) || T.DueLocalEpoch != 100) FailCnt++;
        TS_HEAP_Pop();
        PopCnt++;
    }
    if (PopCnt != 3) FailCnt++;

    /* 3) 删头/中/尾 + 批内重复句柄幂等 + 异源同句柄不删 */
    for (uint32_t i = 0; i < 5; i++)
    {
        E.DueLocalEpoch = 10 * (i + 1);
        E.Handle = i;
        if (!TS_HEAP_Push(&E)) FailCnt++;
    }
    uint32_t HMid = 2;                              /* 删中间（due 30） */
    if (!TS_HEAP_RemoveByHandles(TS_TEST, &HMid, 1)) FailCnt++;
    uint32_t HHead = 0;                             /* 删根（due 10） */
    if (!TS_HEAP_RemoveByHandles(TS_TEST, &HHead, 1)) FailCnt++;
    uint32_t HTail = 4;                             /* 删尾（due 50） */
    if (!TS_HEAP_RemoveByHandles(TS_TEST, &HTail, 1)) FailCnt++;
    uint32_t HDup[2] = {1, 1};
    if (!TS_HEAP_RemoveByHandles(TS_TEST, HDup, 2)) FailCnt++;   /* due 20，批内重复幂等 */
    if (TS_HEAP_RemoveByHandles(TS_TEST, HDup, 2)) FailCnt++;    /* 已删 → false */
    TsRegisterData_t T3;
    if (!TS_HEAP_Top(&T3) || T3.DueLocalEpoch != 40) FailCnt++;
    TS_HEAP_Pop();

    TsRegisterData_t Other = {
        .Head.Source = FEED_EVENTS,
        .DueLocalEpoch = 200,
        .Handle = 7,
    };
    if (!TS_HEAP_Push(&Other)) FailCnt++;
    uint32_t H7 = 7;
    if (TS_HEAP_RemoveByHandles(TS_TEST, &H7, 1)) FailCnt++;         /* 异源不删 */
    if (!TS_HEAP_RemoveByHandles(FEED_EVENTS, &H7, 1)) FailCnt++;    /* 本源删成功 */

    /* 4) 堆满：30 条后第 31 条失败；全清 */
    for (uint32_t i = 0; i < 30; i++)
    {
        E.DueLocalEpoch = 1000 + i;
        E.Handle = 200 + i;
        if (!TS_HEAP_Push(&E)) FailCnt++;
    }
    E.DueLocalEpoch = 2000;
    if (TS_HEAP_Push(&E)) FailCnt++;
    TS_HEAP_Clear();
    if (!TS_HEAP_Empty()) FailCnt++;

    ESP_LOGI("APP", "Heap self-test: %s (fail=%d)", FailCnt == 0 ? "PASS" : "FAIL", FailCnt);
}

static void TS_TestPostTask(void* Parameter)
{
    vTaskDelay(pdMS_TO_TICKS(1000));

    /* G0: 校时（若上电时 OSF=1 则借此回运行态） */
    TsEvtItem_t CalItem = {
        .Data.Calibrate = {
            .Epoch = 1789203876,
            .Head.Source = TS_TEST,
        },
    };
    ESP_LOGI("APP", "G0: calibrate to E0.");
    esp_event_post(TIME_EVENTS, TIME_CALIBRATE, &CalItem, sizeof(CalItem), 0);

    vTaskDelay(pdMS_TO_TICKS(100));

    FdData_t FeedData = {
        .Source = TS_TEST,
        .Type = FEED_TYPE_RESERVE,
        .Weight = 30,
    };

    /* G1: H1 周期 15s，Due E0+12（届时应已因通信中断而停表） */
    TsRegisterData_t Reg = {
        .Head.Source = TS_TEST,
        .DueLocalEpoch = 1789203888,
        .Handle = 1,
        .Periodic = true,
        .PeriodSec = 15,
        .TargetBase = FEED_EVENTS,
        .TargetId = FEED_REQUEST,
        .PayloadLen = sizeof(FdData_t),
    };
    memcpy(&Reg.TargetPayload, &FeedData, sizeof(FdData_t));
    TsEvtItem_t ItemReg = {
        .Data.Register = Reg
    };
    ESP_LOGI("APP", "G1: register H1 (due E0+12).");
    esp_event_post(TIME_EVENTS, TIME_REGISTER, &ItemReg, sizeof(ItemReg), 0);

    /* G2: 8s 时再登记一条 —— 此刻 SDA 应已拔出，RUN 态 GetTime 失败 → 暂停_通信 */
    vTaskDelay(pdMS_TO_TICKS(6900));

    TsRegisterData_t Reg2 = Reg;
    Reg2.Handle = 2;
    Reg2.DueLocalEpoch = 1789203900;
    TsEvtItem_t ItemReg2 = {
        .Data.Register = Reg2
    };
    ESP_LOGI("APP", "G2: register probe (expect CONNECT_FAIL if SDA out).");
    esp_event_post(TIME_EVENTS, TIME_REGISTER, &ItemReg2, sizeof(ItemReg2), 0);

    /* G3: 20s 时取消 H1 —— 暂停_通信 不消费请求队列，应在恢复后才被处理 */
    vTaskDelay(pdMS_TO_TICKS(12000));

    TsEvtItem_t CancelH1 = {
        .Data.Cancel = {
            .Head.Source = TS_TEST,
            .Count = 1,
            .Handles = {1},
        },
    };
    ESP_LOGI("APP", "G3: cancel H1 while pause_i2c may hold the queue.");
    esp_event_post(TIME_EVENTS, TIME_CANCEL, &CancelH1, sizeof(CancelH1), 0);

    vTaskDelete(NULL);
}

static void TS_TestEventHandler(void* pHandlerArgs, esp_event_base_t Base, int32_t Id, void* pEventData)
{
    if (Id == TIME_REJECT && pEventData != NULL)
    {
        TsRejectData_t* Reject = (TsRejectData_t*)pEventData;
        ESP_LOGI("APP", "TIME_REJECT: Source = %s, Handle = %u, Msg = %s", Reject->Source, (unsigned)Reject->Handle, Reject->Msg);
    }
    else
    {
        ESP_LOGI("APP", "TIME_EVENT Id = %d", Id);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    I2C_BusInit(DS3231_SCL, DS3231_SDA);

    Key_Init(BIT(MOTOR_SWITCH) | BIT(DEBUG_MODE_SWITCH));
    Motor_Init(GPIO_NUM_10, GPIO_NUM_11, GPIO_NUM_12);
    Encoder_Init(GPIO_NUM_16, GPIO_NUM_17);

    Feed_Init();

    TS_Heap_SelfTest();

    TS_Init();

    esp_event_handler_register(TIME_EVENTS, ESP_EVENT_ANY_ID, TS_TestEventHandler, NULL);

    xTaskCreate(TS_TestPostTask, "TS_TestPostTask", 2048, NULL, 1, NULL);
    xTaskCreate(Key_MotorSwitchTask, "Key_MotorSwitch", 2048, NULL, 1, NULL);
    xTaskCreate(Key_DebugModeSwitchTask, "Key_DebugModeSwitchTask", 2048, NULL, 1, NULL);
}
