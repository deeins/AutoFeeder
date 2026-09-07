#ifndef __DEBUG_H
#define __DEBUG_H
#include "driver/gpio.h"

typedef struct {
    float   Circle;        /* 触发位置：有符号圈数（Encoder_ClearCount 后从 0 计） */
    int32_t DurationMs; /* 持续时长：-1 = 永久冻结（解冻靠喂食服务自愈重启）；>0 暂缓实现（见「遗留」） */
    uint8_t IsForwardDirStall; /* 是否在正向旋转的时候卡顿 */
    uint8_t MaxTrig;    /* 触发次数上限：本作业内命中几次后规则失效（控制"卡几次"） */
    /* 模块内部状态：已触发计数、本次是否已命中（防同一位置反复重复停） */
} JamRule_t;

typedef enum {
    DB_MODE_NONE, // 不在调试模式
    DB_MODE_SINGLE_DIR_STALL,
    DB_MODE_DOUBLE_DIR_STALL,
    DB_MODE_MAX
} DebugMode_t;

void Debug_ModeRotate(void);

void Debug_SetDebugMode(DebugMode_t DbMode);

DebugMode_t Debug_GetDebugMode(void);

#endif
