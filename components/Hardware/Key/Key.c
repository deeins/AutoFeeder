/**
 * 按键驱动模块
 *
 * 功能：按键引脚初始化、按下状态查询、按下沿检测、等待松开。
 *
 * 接线约定：按键一端接 GPIO、另一端接 GND（按下即低电平），
 * 引脚内部上拉，空闲时读到高电平。
 *
 * 典型用法（主循环轮询）：
 *   1. 启动时 Key_Init(BIT(9));           注册按键（可多次调用注册多个键）
 *   2. 周期调用 Key_GetKeyNum()            查询当前有哪些键被按下
 *   3. 周期调用 Key_GetPressEdge()         查询本次循环以来新按下的键（一次性动作用它）
 *   4. 确认型交互用 Key_WaitRelease()      等待按键松开（会阻塞调用任务）
 *
 * 注意：本模块为轮询实现，不含消抖；机械按键按下瞬间会抖动 5~20ms，
 * 需要在调用侧做 10~20ms 软件消抖，或后续升级为任务 + 事件队列方案。
 */

#include "Key.h"
#include "freertos/FreeRTOS.h"

/** 已注册按键的位掩码（第 n 位为 1 表示 GPIO n 是按键） */
static uint64_t s_Pin = 0;

/** 上一次调用 Key_GetKeyNum 的按位快照，边沿检测依赖它判断"新按下" */
static uint64_t s_LastMask = 0;

/** 按键状态：未按下 / 正在按下 / 已松开 */
enum Key_State
{
    NONE = 0,
    KEY_PRESSING,
    KEY_RELEASE
};

/**
 * 注册按键引脚（可多次调用，内部按位或累加）。
 *
 * @param Pin 按键引脚位掩码，如 BIT(9) | BIT(10)
 *
 * 配置为：输入模式、内部上拉（空闲高电平）、不使能中断（轮询式）。
 * 注意：若按键按下接 3.3V（与 GND 接法相反），需把 pull_up/pull_down 对调，
 * 并把"低电平=按下"的判断改成"高电平=按下"。
 */
void Key_Init(uint64_t Pin)
{
    s_Pin |= Pin;
    const gpio_config_t Cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = Pin,
        .pull_up_en = GPIO_PULLUP_ENABLE,       // 上拉 + 低电平=按下，对应按键一端接 GND
        .pull_down_en = GPIO_PULLDOWN_DISABLE
    };
    gpio_config(&Cfg);
}

/**
 * 查询当前所有被按下的键。
 *
 * @return 位掩码：第 n 位为 1 表示 GPIO n 正被按下，0 表示没有任何键按下
 *
 * 只遍历已注册的引脚（跳过 s_Pin 之外的位），避免读取悬空/未配置引脚。
 * 返回的是"当前时刻"的快照，连续调用且按键未动时结果相同；
 * 若需要"只在按下瞬间触发一次"，用 Key_GetPressEdge()。
 */
uint64_t Key_GetKeyNum(void)
{
    uint64_t KeyNum = 0;

    for (uint8_t i = 0; i < GPIO_NUM_MAX; ++i)
    {
        uint64_t CurBit = BIT(i);
        if (!(CurBit & s_Pin))
        {
            continue;
        }
        if (Key_GetKeyPressState(i))
        {
            KeyNum |= CurBit;
        }
    }
    return KeyNum;
}

/**
 * 查询单个引脚当前是否按下（低电平 = 按下）。
 *
 * @param Pin 引脚编号（GPIO 数字，不是位掩码）
 * @return KEY_PRESSING(1) 表示按下，NONE(0) 表示未按下
 */
uint8_t Key_GetKeyPressState(gpio_num_t Pin)
{
    uint8_t PressState = NONE;
    if (!gpio_get_level(Pin))
    {
        PressState = KEY_PRESSING;
    }
    return PressState;
}

/**
 * 按下沿检测：返回"本次调用以来新按下"的键（按住期间不会重复触发）。
 *
 * @return 位掩码：第 n 位为 1 表示 GPIO n 在本次调用前刚刚被按下
 *
 * 原理：当前快照 & (~上次快照) = 上次没按、这次按下的位。
 * 快照在每次调用后更新，因此轮询间隔就是最小可分辨的按键脉宽：
 * 轮询周期要远小于人类按键时长（一般 50~200ms），建议主循环周期 <= 20ms，
 * 否则可能漏检快速短按。
 * 典型用法：if (Key_GetPressEdge() & BIT(9)) { Feed(); }
 */
uint64_t Key_GetPressEdge(void)
{
    uint64_t Now = Key_GetKeyNum();
    uint64_t Edge = Now & ~s_LastMask;
    s_LastMask = Now;
    return Edge;
}

/**
 * 阻塞等待指定按键松开（确认型交互用，如"按住确认、松开才执行"）。
 *
 * @param Pin 引脚编号（GPIO 数字，不是位掩码）
 * @return KEY_RELEASE(2) 表示检测到按下并已松开；调用时未按下直接返回 NONE(0)
 *
 * 注意：
 *   1. 若引脚处于按下状态，本函数会阻塞直到松开，期间调用它的任务被挂起
 *      （内部以 10ms 轮询，不占死 CPU，但同优先级任务会被饿着）；
 *   2. 松开后额外等待 100ms（消除松手抖动），才会返回；
 *   3. 推荐用 Key_GetPressEdge() 代替本函数做"一次性动作"，本函数仅用于
 *      必须等松手的交互（如长按取消）。
 */
uint8_t Key_WaitRelease(gpio_num_t Pin)
{
    if (gpio_get_level(Pin))
    {
        return NONE;
    }
    while (gpio_get_level(Pin))
    {
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
    return KEY_RELEASE;
}
