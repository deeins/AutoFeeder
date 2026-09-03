#ifndef __FEED_H
#define __FEED_H
#include "esp_event.h"

/*
 * 喂食服务（服务层）—— 定时/手动喂食的执行核心
 *
 * 职责：
 *  - 订阅 [喂食请求事件]，串行执行喂食作业（状态机：就绪→运行→异常⇄运行→结束→就绪）
 *  - 立即喂食去重（防"第二个立即"挤入排队序列）
 *  - 堵粮/出餐口堵塞的公共守卫入口
 *
 * 数据流（两层队列，决策只发生在喂食任务内）：
 *    外部模块 post FEED_REQUEST
 *      → Feed_RequestHandler（esp_event 上下文，只投递不判断）
 *      → s_FdReqQueue（投递队列）
 *      → Feed_HandleRequest（喂食任务上下文：立即去重决策、拒绝回灌）
 *      → s_FdQueue（作业队列）
 *      → 就绪态取出作业执行
 *
 * 对应设计文档：《模块设计/服务/喂食服务.md》
 */

#define FEED_ERROR_RETRY_TIMES 3    /* 卡粮反转自愈的最大次数 */

ESP_EVENT_DECLARE_BASE(FEED_EVENTS);
/* 声明放头文件，定义在.c里——若在.h里定义，被多个.c包含会产生多重定义 */

/* FEED_EVENTS 事件 ID（int32_t 值，从 0 起） */
enum {
    FEED_REQUEST,       /* 喂食请求事件：外部 → 喂食服务（载荷 FdData_t） */
    FEED_REJECT,        /* 喂食拒绝事件：喂食服务 → 外部（载荷 FdMsgData_t，携带 source 回灌） */
    FEED_START,         /* 喂食开始事件：喂食服务 → 外部订阅者 */
    FEED_END,           /* 喂食结束事件：喂食服务 → 外部订阅者 */
    FEED_BLOCK,         /* 喂食阻塞事件：喂食服务 → 外部订阅者 */
};

/* 喂食类型：如实标记请求的触发性质（供日志/订阅方区分），不因内部逻辑失真 */
typedef enum {
    FEED_TYPE_RESERVE,      /* 预约：定时器到点触发的计划喂食 */
    FEED_TYPE_IMMEDIATE     /* 立即：按键/App 手动喂食 */
} FeedType_t;

/*
 * 事件来源：字符串常量符号（同 esp_event base 机制）
 *  - 各来源模块自己 DEFINE/DECLARE，互不引用对方符号，链接器防重名
 *  - 比较用 ==（指针比较同一 extern 符号），不要 strcmp
 *  - 例：app.c 里 FEED_SOURCE_DEFINE(FEED_SRC_APP);  发请求时 .Source = FEED_SRC_APP
 */
typedef const char* FeedSource_t;
#define FEED_SOURCE_DEFINE(id) FeedSource_t const id = #id     /* 放各来源模块 .c（全工程唯一） */
#define FEED_SOURCE_DECLARE(id) extern FeedSource_t const id   /* 放各来源模块 .h */

/* 喂食请求/作业载荷 */
typedef struct {
    FeedType_t   Type;       /* 预约 / 立即 */
    FeedSource_t Source;     /* 发起者符号（拒绝/结果回灌时定位发起方） */
    uint8_t      Weight;     /* 克重 */
} FdData_t;

/* 喂食结果/通知载荷（拒绝、阻塞等带文本信息的事件） */
typedef struct {
    FdData_t FdData;         /* 原始请求信息（回灌用） */
    char*    Msg;            /* 提示文本；esp_event 深拷贝只拷指针不拷字符串，须传静态/字面量 */
} FdMsgData_t;

/* 红外消息：红外驱动模块未写，先占位（0=卡粮 1=出餐口堵）；模块成型后移到驱动侧头文件 */
typedef struct {
    uint8_t type;
} IR_Data_t;

/* 喂食状态机状态 */
typedef enum {
    FEED_ST_NONE = 0,       /* 无效态（兜底用，正常运行不落在此态） */
    FEED_ST_READY,          /* 就绪：等作业（取作业队列） */
    FEED_ST_RUNNING,        /* 运行：电机转圈推进中 */
    FEED_ST_ERROR,          /* 异常：卡粮反转自愈中 */
    FEED_ST_END             /* 结束：收尾（清立即标志/失败置闸门/预约续期/发事件） */
} FdState_t;

/*
 * 状态机"动作"的结果码（语义 = 状态转移图出边上的转移条件）
 * 每 tick 由 Act 返回一个，转移层据此集中评估是否切状态
 */
typedef enum {
    FD_RES_NONE,              /* 无结果（不应出现；转移层兜底驻留） */
    FD_RES_IN_PROGRESS,       /* 长停留态通用：动作仍在推进（转圈/观察恢复/收尾中）→ 停留原地 */
    FD_RES_JOB_NONE,          /* 就绪：队列空 → 停留就绪 */
    FD_RES_JOB_TAKEN,         /* 就绪：取出作业 → 运行 */
    FD_RES_TARGET_REACHED,    /* 运行：转数到达 → 结束 */
    FD_RES_NO_PULSE,          /* 运行：N tick 无脉冲（该转没转）→ 异常 */
    FD_RES_RETRY_OK,          /* 异常：反转自愈恢复 → 运行 */
    FD_RES_RETRY_FAIL,        /* 异常：自愈失败（重试计数在转移层）→ 未达上限留异常/达上限结束 */
    FD_RES_DONE               /* 结束：收尾完成 → 就绪 */
} FdActRes_t;

/* 初始化喂食服务：创建队列、注册事件订阅（在 app_main 调用一次） */
void Feed_Init(void);

/* 喂食任务单 tick：红外守卫 + 请求决策 + 状态机动作/转移（由周期任务循环调用） */
void Feed_Run(void);

#endif
