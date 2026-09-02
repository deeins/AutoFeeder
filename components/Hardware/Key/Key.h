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
void Key_Init(uint64_t pin);

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
uint8_t Key_GetKeyPressState(gpio_num_t pin);

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
uint8_t Key_WaitRelease(gpio_num_t pin);

#endif
