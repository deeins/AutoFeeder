/**
 * 按键驱动模块接口
 *
 * 使用流程：Key_Init() 注册引脚 → 主循环轮询 Key_GetKeyNum() / Key_GetPressEdge()。
 *
 * 接线约定：按键一端接 GPIO、另一端接 GND，引脚内部上拉（低电平 = 按下）。
 *
 * 常见用法：
 *   - 一键一动作（喂食、切换）→ Key_GetPressEdge()（按下沿，只触发一次）
 *   - 按住持续动作（连续出粮）→ Key_GetKeyNum()（电平查询）
 *   - 松开才执行（确认型交互）→ Key_WaitRelease()
 */

#ifndef __KEY_H
#define __KEY_H
#include "driver/gpio.h"

/**
 * 注册按键引脚，可多次调用注册多个键。
 *
 * @param pin 引脚位掩码，如 BIT(9) | BIT(10)
 */
void Key_Init(uint64_t Pin);

/**
 * 查询当前所有被按下的键。
 *
 * @return 位掩码：第 n 位为 1 表示 GPIO n 正被按下；0 表示都没有按下
 */
uint64_t Key_GetKeyNum(void);

/**
 * 查询单个引脚当前是否按下。
 *
 * @param pin 引脚编号（GPIO 数字，不是位掩码）
 * @return 非 0 表示按下，0 表示未按下
 */
uint8_t Key_GetKeyPressState(gpio_num_t Pin);

/**
 * 按下沿检测：本次调用以来新按下的键，按住期间不重复触发。
 *
 * @return 位掩码：第 n 位为 1 表示 GPIO n 刚被按下
 *
 * 一次性动作（喂食、切换功能）用它，避免主循环每轮重复触发。
 */
uint64_t Key_GetPressEdge(void);

/**
 * 阻塞等待指定按键松开。
 *
 * @param pin 引脚编号（GPIO 数字，不是位掩码）
 * @return 2(KEY_RELEASE) 表示按下后已松开；调用时未按下直接返回 0
 *
 * 警告：会阻塞调用任务直到松开，仅在确认型交互使用；普通动作优先 Key_GetPressEdge()。
 */
uint8_t Key_WaitRelease(gpio_num_t Pin);

/**
 * 单击检测（便捷封装）：轮询式检测单键「按下 → 消抖确认 → 触发回调 → 等待松开」。
 *
 * 在按键任务中周期调用，结构：
 *   1. 检测到按下 → vTaskDelay(20ms) 消抖 → 二次确认仍按下才触发回调（沿触发，按住不重复）；
 *   2. 回调返回后阻塞等待松开（10ms 轮询）；
 *   3. 每轮末尾 vTaskDelay(10ms) 兜底轮询节奏（同时兼作松手抖动窗口）。
 *
 * @param Pin      引脚编号（GPIO 数字）
 * @param CallBack 消抖确认后调用的回调函数（执行于调用任务上下文，须短促非阻塞）
 *
 * 语义注意：回调在「松开前」触发（按下沿 + 消抖确认），不是「完整单击后」触发；
 * 将来若需区分单击/长按或松开才执行，此封装不适用，需另行扩展。
 */
void Key_SingleClickCheck(gpio_num_t Pin, void CallBack(void));

#endif
