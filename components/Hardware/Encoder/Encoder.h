#ifndef __ENCODER_H
#define __ENCODER_H
#include "driver/gpio.h"

/*
 * 电机尾部 5 线霍尔编码器（A/B 两相，90° 正交）
 *  - A 相：脉冲计数（喂食转数到达 / 卡粮"该转没转"检测）
 *  - B 相：判向预留（已接线，未启用；启用方式见 Encoder.c Init 内注释块）
 *
 * ⚠️ 判向接口使用铁律（写代码前必读）：
 *  方向信息的正确来源 = "有符号计数变化量"，不是软件读 B 电平快照——
 *  B 随旋转持续翻转，任意时刻的电平与方向无对应关系；只有"A 边沿发生瞬间的
 *  B 电平"才能判向，那是 PCNT 硬件 level action 干的活，软件轮询补不了。
 *
 *  预期消费方：喂食服务"异常态"反转自愈需要验证"确实在反转"时，
 *  先启用 Encoder.c 里的方向解码，再用两次 Encoder_GetCount() 的差值符号判定。
 *  届时计数变为有符号，喂食任务的到达判定需改按差值/绝对值处理。
 */

/* 初始化：Pin1 = A 相（计数），Pin2 = B 相（判向预留） */
void Encoder_Init(gpio_num_t Pin1, gpio_num_t Pin2);

/* 当前累计脉冲数（未开判向前为正值计数） */
int Encoder_GetCount(void);

/* 折算"电机对外输出轴"圈数（定量轮所在轴）：脉冲 ÷ (每输入圈脉冲 × 减速比) = ÷(7×100)=÷700 */
float Encoder_GetOutputRotCount(void);

/* 计数清零（每作业开始时调用一次，规避 ±10000 上限自动清零的锚定问题） */
void Encoder_ClearCount(void);

/* 通电 + 开数（幂等，重复调用安全） */
void Encoder_StartCount(void);

/* 停数 + 断电（幂等，重复调用安全） */
void Encoder_StopCount(void);

#endif
