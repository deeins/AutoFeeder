#include "Encoder.h"
#include "driver/pulse_cnt.h"

/* ===== 测试注入开关：模拟编码器读数（正式版把下面宏置 0 即恢复真实计数）===== */
#define ENCODER_TEST_INJECT 1

#define PULSE_CNT_PER_MOTOR_ROUND 7    /* 编码器所在轴每转脉冲数（商家参数：电机输入侧/最小齿轮） */
#define MOTOR_GEAR_RATIO            100 /* 减速比：输入侧转 100 圈 = 对外输出轴（定量轮）1 圈 */

static gpio_num_t s_Pin1;             /* A 相：计数脉冲 */
static gpio_num_t s_Pin2;             /* B 相：判向预留（90° 正交，已接线，未启用） */

static bool s_Counting = false;       /* 模块内计数状态：Start/Stop 幂等开关的唯一状态源 */

#if ENCODER_TEST_INJECT
static int s_TestPulse = -1;          /* >=0 时用假脉冲数替代真实计数；-1 = 真实编码器 */
#endif

static pcnt_unit_handle_t    g_Unit;  /* 模块内句柄：Init 创建后供本文件所有函数使用 */
static pcnt_channel_handle_t g_Chan;

void Encoder_Init(gpio_num_t Pin1, gpio_num_t Pin2)
{
    s_Pin1 = Pin1;
    s_Pin2 = Pin2;

    /* A/B 都配成带上拉的输入：编码器多为开漏输出，悬空电平要有确定态 */
    gpio_config_t Cfg = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT(Pin1) | BIT(Pin2),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE
    };
    gpio_config(&Cfg);

    /* 1. 计数单元：定范围（计数器到上限会自动清零回 0；配 ClearCount 每作业清一次规避） */
    pcnt_unit_config_t UnitCfg = {
        .high_limit = 10000,
        .low_limit  = -10000,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&UnitCfg, &g_Unit));

    /* 2. 通道：A 相接 edge（数脉冲），B 相接 level（判向预留） */
    pcnt_chan_config_t ChanCfg = {
        .edge_gpio_num  = Pin1,
        .level_gpio_num = Pin2,
    };
    ESP_ERROR_CHECK(pcnt_new_channel(g_Unit, &ChanCfg, &g_Chan));

    /* 3. A 相上升沿 +1，下降沿不动（未开判向前 = 纯正向计数） */
    // ESP_ERROR_CHECK(pcnt_channel_set_edge_action(g_Chan,
    //     PCNT_CHANNEL_EDGE_ACTION_INCREASE,   /* pos edge */
    //     PCNT_CHANNEL_EDGE_ACTION_HOLD));     /* neg edge */

    /* 4. 毛刺滤波：<1µs 的窄脉冲当噪声扔掉（100kHz 上限半周期 5µs 仍能过；实际转速远低于此） */
    pcnt_glitch_filter_config_t FilterCfg = { .max_glitch_ns = 1000 };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(g_Unit, &FilterCfg));

    /* 5. 方向解码预留：B 相消费方（喂食异常态反转自愈验证）出现后，解除下面两段注释启用：
     *    - edge 动作改成 A 双沿计数；
     *    - level 动作让 B 判向：B 高=正转、B 低=反转（加减互换）。
     *    ⚠️ 启用后 GetCount/GetOutputRotCount 变为"有符号"计数，
     *       喂食任务的到达判定要改按两次取数的差值（绝对值）做。
     */
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(g_Chan,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE,
        PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(g_Chan,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP,       /* B 高：保持计数方向 */
        PCNT_CHANNEL_LEVEL_ACTION_INVERSE));  /* B 低：加减互换（反向） */
}

/* 测试注入：设置假脉冲数（>=0 生效，-1 恢复真实计数）。ENCODER_TEST_INJECT=0 时为空操作。 */
void Encoder_TestSetPulse(int Pulse)
{
#if ENCODER_TEST_INJECT
    s_TestPulse = Pulse;
#endif
}

/* 当前累计脉冲数（未开判向时为正值计数；测试注入开启时返回假读数） */
int Encoder_GetCount(void)
{
#if ENCODER_TEST_INJECT
    if (s_TestPulse >= 0)
    {
        return s_TestPulse;
    }
#endif
    int Count = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(g_Unit, &Count));
    return Count;
}

/*
 * 折算"电机对外输出轴"圈数（定量轮所在轴，喂食量化的物理参照）：
 *   输出圈数 = 脉冲 / (每输入轴圈脉冲 × 减速比) = PulseCnt / (7 × 100) = PulseCnt / 700
 * 浮点返回，直接套 克数 = 输出圈数 × 每输出圈克数，无整除丢余
 */
float Encoder_CountToOutputRot(int PulseCnt)
{
    return PulseCnt / (float)(PULSE_CNT_PER_MOTOR_ROUND * MOTOR_GEAR_RATIO) / 2;
}

/* 量化脉冲变化角度，正向旋转为正角度 */
float Encoder_GetDeltaAngle(int TargetPulse, int StartPulse)
{
    return Encoder_CountToOutputRot(TargetPulse - StartPulse) * 360;
}

/* 便捷版本：用当前计数值换算（若调用方已持有快照，建议用 CountToOutputRot 复用，避免二次读数） */
float Encoder_GetOutputRotCount(void)
{
    return Encoder_CountToOutputRot(Encoder_GetCount());
}

void Encoder_ClearCount(void)
{
    ESP_ERROR_CHECK(pcnt_unit_clear_count(g_Unit));
}

void Encoder_StartCount(void)     /* = 通电 + 开数（幂等，重复调用安全） */
{
    if (s_Counting) return;
    ESP_ERROR_CHECK(pcnt_unit_enable(g_Unit));
    ESP_ERROR_CHECK(pcnt_unit_start(g_Unit));
    s_Counting = true;
}

void Encoder_StopCount(void)      /* = 停数 + 断电（幂等，重复调用安全） */
{
    if (!s_Counting) return;
    ESP_ERROR_CHECK(pcnt_unit_stop(g_Unit));
    ESP_ERROR_CHECK(pcnt_unit_disable(g_Unit));
    s_Counting = false;
}
