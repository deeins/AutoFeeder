#include "Display.h"
#include "Feed.h"
#include "TimeScheduler.h"
#include "OLED.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/*
 * 显示模块（单线程：事件回调只投递、查表/更新槽/渲染全在显示任务内，无锁）
 *  - 三来源槽 + 优先级：喂食故障 10 > 喂食中 8 > 校时/通信故障 6 > 网络 4（网络槽预留）
 *  - 文本级去重：渲染文本不变不发 I2C；多帧内容按帧时长走可变 tick，静态内容 portMAX_DELAY 阻塞
 *  - 本地失败退避：单次失败跳帧 + 驱动限流日志；连续 3 次判离线（停动画帧），每 10s 试写，成功恢复
 */

#define DISPLAY_TEXT_MAX       32       /* UTF-8 8 汉字（3B/字）+ NUL，留裕量 */
#define DISPLAY_SLOT_COUNT     3
#define DISPLAY_ROW            1        /* 状态行 = 第 2 个逻辑行（0 基，4 行取中） */
#define DISPLAY_QUEUE_LEN      8
#define DISPLAY_TICK_MIN_MS    250      /* 帧时长 clamp 下限 */
#define DISPLAY_TICK_MAX_MS    1000     /* 帧时长 clamp 上限 */
#define DISPLAY_FEED_HOLD_MS   400      /* 喂食中动画帧时长 */
#define DISPLAY_OFFLINE_LIMIT  3        /* 连续失败 N 次 → 显示离线 */
#define DISPLAY_RETRY_MS       10000    /* 离线后试写周期 */

static const char* Display_TAG = "Display";

typedef enum {
    SLOT_FEED = 0,
    SLOT_TIME,
    SLOT_NET,
} DisplaySlotId_t;

typedef struct {
    const char* Text;
    uint16_t    HoldMs;
} DisplayFrame_t;

typedef struct {
    const DisplayFrame_t* Frames;       /* NULL → 动态文本（用槽内 Text） */
    uint8_t               Count;        /* 帧数 */
    uint8_t               Priority;     /* 越大越优先 */
} DisplayContent_t;

typedef struct {
    esp_event_base_t        Base;
    int32_t                 Id;
    uint8_t                 Slot;
    const DisplayContent_t* Content;    /* NULL → 清除该槽 */
} DisplayMapEntry_t;

/* 内容描述符表 */
static const DisplayFrame_t s_FeedFrames[] = {
    { "喂食中.",   DISPLAY_FEED_HOLD_MS },
    { "喂食中..",  DISPLAY_FEED_HOLD_MS },
    { "喂食中...", DISPLAY_FEED_HOLD_MS },
};

static const DisplayContent_t s_C_Feeding   = { s_FeedFrames, 3, 8 };    /* 喂食中动画 */
static const DisplayContent_t s_C_FeedFault = { NULL, 1, 10 };           /* 动态 Msg（出粮口堵塞等） */
static const DisplayContent_t s_C_TimeFault = { NULL, 1, 6 };            /* 动态 Msg（时间无效/通信故障） */

/* 事件 → 槽 + 内容 映射表（外部加状态：加一行即可，渲染逻辑不动）
 * 注：FEED_EVENTS/TIME_EVENTS 是 const 指针变量、非编译期常量，Base 域由 Display_MapInit 运行期填 */
#define DISPLAY_MAP_FEED_COUNT 4    /* 表前 4 条属 FEED_EVENTS，其余属 TIME_EVENTS */

static DisplayMapEntry_t s_Map[] = {
    { NULL, FEED_START,        SLOT_FEED, &s_C_Feeding   },
    { NULL, FEED_RECOVER,      SLOT_FEED, &s_C_Feeding   },
    { NULL, FEED_BLOCK,        SLOT_FEED, &s_C_FeedFault },
    { NULL, FEED_END,          SLOT_FEED, NULL           },
    { NULL, TIME_INVALID,      SLOT_TIME, &s_C_TimeFault },
    { NULL, TIME_CONNECT_FAIL, SLOT_TIME, &s_C_TimeFault },
    { NULL, TIME_VALID,        SLOT_TIME, NULL           },
    { NULL, TIME_CONNECT_OK,   SLOT_TIME, NULL           },
};

static void Display_MapInit(void)
{
    for (uint32_t i = 0; i < DISPLAY_MAP_FEED_COUNT; i++)
    {
        s_Map[i].Base = FEED_EVENTS;
    }
    for (uint32_t i = DISPLAY_MAP_FEED_COUNT; i < sizeof(s_Map) / sizeof(s_Map[0]); i++)
    {
        s_Map[i].Base = TIME_EVENTS;
    }
}

typedef struct {
    char                    Text[DISPLAY_TEXT_MAX];
    const DisplayContent_t* Content;    /* NULL = 空槽 */
    uint8_t                 FrameIndex;
} DisplaySlot_t;

/* 队列条目：载荷按事件族各自拷贝成常驻形式（回调上下文只做拷贝，不做查表/渲染） */
typedef struct {
    esp_event_base_t Base;
    int32_t          Id;
    const char*      FdMsg;             /* FEED_BLOCK 的 Msg 指针（须为静态/字面量，与 FdMsgData_t 契约一致） */
    char             Text[DISPLAY_TEXT_MAX];    /* TIME 事件裸字符串载荷 */
} DisplayEvt_t;

static DisplaySlot_t s_Slots[DISPLAY_SLOT_COUNT];
static QueueHandle_t s_Queue = NULL;
static char s_LastText[DISPLAY_TEXT_MAX];   /* 最近一次成功写屏的文本（去重用） */
static bool s_Offline = false;
static uint8_t s_ConsecFail = 0;

static void Display_EventHandler(void* Args, esp_event_base_t Base, int32_t Id, void* Data)
{
    if (Data == NULL)
    {
        return;
    }

    DisplayEvt_t Evt = {
        .Base = Base,
        .Id = Id,
    };

    if (Base == FEED_EVENTS)
    {
        /* 只有 FEED_BLOCK 带 FdMsgData_t（FEED_START/END 载荷是 FdData_t，不可按 FdMsgData_t 读） */
        if (Id == FEED_BLOCK)
        {
            Evt.FdMsg = ((FdMsgData_t*)Data)->Msg;
        }
    }
    else if (Base == TIME_EVENTS)
    {
        /* TimeScheduler 的 TIME_* 事件以裸字符串为载荷（esp_event 已值拷贝进事件循环） */
        strncpy(Evt.Text, (const char*)Data, sizeof(Evt.Text) - 1);
        Evt.Text[sizeof(Evt.Text) - 1] = '\0';
    }
    else
    {
        return;
    }

    xQueueSend(s_Queue, &Evt, 0);
}

static void Display_UpdateSlot(const DisplayEvt_t* Evt)
{
    for (uint32_t i = 0; i < sizeof(s_Map) / sizeof(s_Map[0]); i++)
    {
        if (s_Map[i].Base != Evt->Base || s_Map[i].Id != Evt->Id)
        {
            continue;
        }

        DisplaySlot_t* Slot = &s_Slots[s_Map[i].Slot];
        Slot->Content = s_Map[i].Content;
        Slot->FrameIndex = 0;
        Slot->Text[0] = '\0';

        if (Slot->Content != NULL && Slot->Content->Frames == NULL)
        {
            const char* Msg = (Evt->Base == FEED_EVENTS) ? Evt->FdMsg : Evt->Text;
            if (Msg != NULL)
            {
                strncpy(Slot->Text, Msg, sizeof(Slot->Text) - 1);
                Slot->Text[sizeof(Slot->Text) - 1] = '\0';
            }
        }

        ESP_LOGI(Display_TAG, "Slot %d <- event %d (%s)",
                 (int)s_Map[i].Slot, (int)Evt->Id,
                 Slot->Content == NULL ? "clear" : (Slot->Content->Frames == NULL ? Slot->Text : "frames"));
        return;
    }
}

/* 取优先级最高的非空槽；同分按 feed > time > net（表序） */
static int Display_TopSlot(void)
{
    int Top = -1;
    for (int i = 0; i < DISPLAY_SLOT_COUNT; i++)
    {
        if (s_Slots[i].Content == NULL)
        {
            continue;
        }
        if (Top < 0 || s_Slots[i].Content->Priority > s_Slots[Top].Content->Priority)
        {
            Top = i;
        }
    }
    return Top;
}

static const char* Display_CurrentText(int Top)
{
    if (Top < 0)
    {
        return "待机中";    /* 全空默认 */
    }
    const DisplaySlot_t* Slot = &s_Slots[Top];
    if (Slot->Content->Frames == NULL)
    {
        return Slot->Text;
    }
    return Slot->Content->Frames[Slot->FrameIndex].Text;
}

static TickType_t Display_WaitTicks(int Top)
{
    if (s_Offline)
    {
        return pdMS_TO_TICKS(DISPLAY_RETRY_MS);
    }
    if (Top >= 0 && s_Slots[Top].Content->Frames != NULL && s_Slots[Top].Content->Count > 1)
    {
        uint16_t Hold = s_Slots[Top].Content->Frames[s_Slots[Top].FrameIndex].HoldMs;
        if (Hold < DISPLAY_TICK_MIN_MS)
        {
            Hold = DISPLAY_TICK_MIN_MS;
        }
        if (Hold > DISPLAY_TICK_MAX_MS)
        {
            Hold = DISPLAY_TICK_MAX_MS;
        }
        return pdMS_TO_TICKS(Hold);
    }
    return portMAX_DELAY;   /* 单帧/静态/空闲：零刷新、零 CPU */
}

static void Display_Refresh(int Top)
{
    const char* Text = Display_CurrentText(Top);

    if (!s_Offline && strcmp(Text, s_LastText) == 0)
    {
        return;             /* 文本级去重：不变不写 I2C */
    }

    if (OLED_ShowTextLine(DISPLAY_ROW, Text) == ESP_OK)
    {
        s_ConsecFail = 0;
        if (s_Offline)
        {
            s_Offline = false;
            ESP_LOGI(Display_TAG, "Display recovered, resume live refresh.");
        }
        strncpy(s_LastText, Text, sizeof(s_LastText) - 1);
        s_LastText[sizeof(s_LastText) - 1] = '\0';
    }
    else
    {
        if (s_ConsecFail < UINT8_MAX)
        {
            s_ConsecFail++;
        }
        if (s_ConsecFail >= DISPLAY_OFFLINE_LIMIT && !s_Offline)
        {
            s_Offline = true;
            ESP_LOGI(Display_TAG, "Display offline after %u failures, retry every %d ms.",
                     (unsigned)s_ConsecFail, DISPLAY_RETRY_MS);
        }
    }
}

static void Display_Task(void* Parameter)
{
    DisplayEvt_t Evt;
    for (;;)
    {
        int Top = Display_TopSlot();
        TickType_t Timeout = Display_WaitTicks(Top);

        if (xQueueReceive(s_Queue, &Evt, Timeout) == pdTRUE)
        {
            Display_UpdateSlot(&Evt);   /* 内容切换：帧号在其内清零 */
        }
        else if (!s_Offline && Top >= 0 && s_Slots[Top].Content->Frames != NULL && s_Slots[Top].Content->Count > 1)
        {
            s_Slots[Top].FrameIndex = (s_Slots[Top].FrameIndex + 1) % s_Slots[Top].Content->Count;
        }

        Display_Refresh(Display_TopSlot());    /* 事件后顶槽可能变化，重取 */
    }
}

void Display_Init(void)
{
    Display_MapInit();

    if (OLED_Init() != ESP_OK)
    {
        /* 不阻塞启动：显示任务照常起，靠退避机制持续试写 */
        ESP_LOGI(Display_TAG, "OLED init failed, display starts with backoff retry.");
    }

    s_Queue = xQueueCreate(DISPLAY_QUEUE_LEN, sizeof(DisplayEvt_t));
    assert(s_Queue != NULL);

    ESP_ERROR_CHECK(esp_event_handler_register(FEED_EVENTS, ESP_EVENT_ANY_ID, Display_EventHandler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(TIME_EVENTS, ESP_EVENT_ANY_ID, Display_EventHandler, NULL));

    xTaskCreate(Display_Task, "Display_Task", 4096, NULL, 2, NULL);
}
