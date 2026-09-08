#ifndef __TIME_SCHEDULER_H
#define __TIME_SCHEDULER_H
#include "esp_event_base.h"

#define TS_PAYLOAD_MAX 10
#define TS_CANCEL_MAX 10
#define TS_QUEUE_REQUEST_MAX 20

ESP_EVENT_DECLARE_BASE(TIME_EVENTS);

enum {
    TIME_REGISTER,
    TIME_CANCEL,
    TIME_CALIBRATE,
    TIME_REJECT,
    TIME_INVALID,
    TIME_VALID,
    TIME_CONNECT_FAIL,
    TIME_CONNECT_OK,
};

/* 公共头：各请求载荷首字段一致（仅 Source 同构，队列条目统一从头部读） */
typedef struct {
    esp_event_base_t Source;   /* 发起者署名（答复回灌定位），用 ESP_EVENT_*_BASE 宏定义符号 */
} TsReqHead_t;

typedef struct {               /* TIME_REGISTER：调度规格 */
    TsReqHead_t Head;
    uint32_t    Handle;        /* 预约身份句柄：发起方生成（广播模型无回执），同句柄再登记 = 覆盖更新；
                                * 循环条目 re-arm 沿用原句柄（App 才能撤循环预约） */
    uint32_t    DueLocalEpoch; /* 下次触发时刻（**本地墙钟** epoch，非 UTC）。本地时刻 epoch 之间差值
                                * 与 86400s 周期运算与时区无关；调度域全链路本地基准，见决策 8 */
    bool        Periodic;      /* 一次性 / 循环 */
    uint32_t    PeriodSec;     /* 循环周期（无 DST 时 86400s 后推天然正确） */
    esp_event_base_t TargetBase;   /* 到期转发的目标事件族（全工程唯一 extern 符号） */
    int32_t          TargetId;     /* 目标事件 ID */
    uint16_t         PayloadLen;
    uint8_t          TargetPayload[TS_PAYLOAD_MAX];  /* 目标载荷（内嵌定长：esp_event 值拷贝，
                                                      * 指针会悬垂——同 Msg 静态/字面量先例）。
                                                      * 由发起方组装（黑盒，定时服务不认识结构），
                                                      * 到点弹堆 → 原样 post → 条目销毁 */
} TsRegisterData_t;

typedef struct {               /* TIME_CANCEL：句柄列表批量取消 */
    TsReqHead_t Head;
    uint8_t     Count;         /* 实际数量（≤ 数组容量，见下注） */
    uint32_t    Handles[TS_CANCEL_MAX];   /* 句柄列表（容量 = 定长 struct 的技术约束，非协议语义；
                                           * Count=0 = 取消设备当前全部定时条目） */
} TsCancelData_t;

typedef struct {               /* TIME_CALIBRATE：校时（新时间 = **本地墙钟** epoch，全链路唯一跨基准转换点在发送侧，见决策 8） */
    TsReqHead_t Head;
    uint32_t    Epoch;         /* 本地时刻 epoch（墙钟语义）。来源：SNTP 同步（网络模块：UTC epoch + NVS
                                * 时区偏移后发出，Source=NET）/ 出厂校准（Source=FACTORY，首次上电/生产写
                                * 基准时间清 OSF）/ App/OLED 手动（Source 各定）。误差 ≤1s（与驱动 ±1~2s 接受区间一致） */
} TsCalibrateData_t;

typedef struct {               /* TIME_REJECT：公共头 + 提示（不带原请求全文） */
    esp_event_base_t Source;   /* 原发起者（比对收回） */
    uint32_t         Handle;   /* 涉及条目句柄（登记/取消时有效，可空） */
    const char*      Msg;      /* 提示文本；esp_event 深拷贝只拷指针，须传静态/字面量 */
} TsRejectData_t;

typedef enum {
    TS_RT_REGISTER,
    TS_RT_CANCEL,
    TS_RT_CALIBRATE,
} TsRequestType_t;

typedef struct {
    int32_t Id; // 事件ID，范围只有 TIME_REGISTER, TIME_CANCEL, TIME_CALIBRATE
    union {
        TsRegisterData_t  Register;
        TsCancelData_t    Cancel;
        TsCalibrateData_t Calibrate;
    } Data;
} TsEvtItem_t;

typedef enum {
    TS_ST_INIT,
    TS_ST_RUN,
    TS_ST_PAUSE_TIME,
    TS_ST_PAUSR_I2C,
} TsState_t;

typedef enum {
    TS_RES_NONE,
    TS_RES_INIT_DONE,
    TS_RES_INIT_I2C_FAIL,
    TS_RES_TIME_INVALID,
    TS_RES_TIME_VALID,
    TS_RES_I2C_FAIL,
    TS_RES_I2C_OK,
    TS_RES_IN_PROGRESS,
} TsActRes_t;

void TS_Init(void);

void TS_RunTask(void);

#endif
