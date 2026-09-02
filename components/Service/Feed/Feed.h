#ifndef __FEED_H
#define __FEED_H
#include "esp_event.h"

#define FEED_ERROR_RETRY_TIMES 3

ESP_EVENT_DECLARE_BASE(FEED_EVENTS); // 声明放头文件，定义在.c里，如果在.h里定义，会因为其他文件包含了本文件内容，导致多重定义

enum {
    FEED_REQUEST,
    FEED_REJECT,
    FEED_START,
    FEED_END,
    FEED_BLOCK,
};

typedef enum {
    FEED_TYPE_RESERVE,
    FEED_TYPE_IMMEDIATE
} FeedType_t;

typedef const char* FeedSource_t;
#define FEED_SOURCE_DEFINE(id) FeedSource_t const id = #id
#define FEED_SOURCE_DECLARE(id) extern FeedSource_t const id

typedef struct {
    FeedType_t Type;
    FeedSource_t Source;
    uint8_t Weight; // 单位克
} FdData_t;

typedef struct {
    FdData_t FdData;
    char* Msg;
} FdMsgData_t;

typedef struct {
    uint8_t Occupy; // 占位
} IR_Data_t;

typedef enum {
    FEED_ST_NONE = 0,
    FEED_ST_READY,
    FEED_ST_RUNNING,
    FEED_ST_ERROR,
    FEED_ST_END
} FdState_t;

typedef enum {
    FD_RES_NONE,              /* 没有输入正确的喂食状态 */
    FD_RES_IN_PROGRESS,       /* 长停留态通用：动作仍在推进中（运行态转圈中/异常态观察恢复中/收尾中），转移层停留原地 */
    FD_RES_JOB_NONE,          /* 就绪：队列空 */
    FD_RES_JOB_TAKEN,         /* 就绪：取出作业 */
    FD_RES_TARGET_REACHED,    /* 运行：转数到达 */
    FD_RES_NO_PULSE,          /* 运行：N tick 无脉冲（该转没转，tick 轮询发现） */
    FD_RES_RETRY_OK,          /* 异常：反转自愈恢复 */
    FD_RES_RETRY_FAIL,        /* 异常：自愈失败（次数在 NextState 里数） */
    FD_RES_DONE               /* 结束：收尾完成 */
} FdActRes_t;

void Feed_Init(void);

void Feed_Run(void);

#endif
