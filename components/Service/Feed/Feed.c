#include "Feed.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "Motor.h"
#include "Encoder.h"

/*
 * 喂食服务实现（对应《模块设计/服务/喂食服务.md》）
 *
 * 关键纪律：
 *  - tick 状态机：Feed_Run 每 tick 至多执行一次动作、一次转移，case 函数必须非阻塞短切片
 *  - 单写者零锁：立即标志 s_bFdQueueOwnedImdtSource 的查/置/清全部只在喂食任务上下文发生
 *    （Feed_HandleRequest 置位、Feed_EndHandler 收尾清除），esp_event 回调只投递不碰标志
 *  - 守卫打断：红外消息（堵粮）由 Feed_Run 顶部带超时接收，是唯一"动作外"转移路径
 *
 * 三层职责划分（写代码前对照，防错位）：
 *  - 感知层（Feed_ReadyHandler / RunningHandler / EndHandler）：
 *    每 tick 幂等查询/判断，只返回结果码。查询式动作可重复执行，绝不产生硬件副作用。
 *  - 转移层（Feed_StateTransition）：
 *    ① 纯映射：结果码 → 下一状态（含重试计数 s_RetryCnt）；
 *    ② 一次性副作用："进入新状态瞬间"的硬件开关（电机/编码器启停）与转移日志——
 *       只在状态真变化的那 tick 执行一次，是"沿触发"，与感知层的"每 tick 轮询"互补。
 *  - 守卫例外（Feed_OnBlockage）：堵粮等"动作外"打断，不走 Act 结果链，直接停电机并切状态。
 */

/* 校准常量：每"电机对外输出圈"（定量轮 1 圈）出粮克数——暂定值，用电子秤实测后替换 */
#define GRAM_PER_OUTPUT_ROT 10

// 异常_反转自愈状态反转的角度，角度制，以运行态进异常时的脉冲作为锚点
#define RECOVER_REVERSE_ANGLE -10
// 复位后继续转动多少度，以运行态进异常时的脉冲作为锚点
#define RECOVER_ANGLE 5

/*
 * 克重换算（服务层内部纯函数）：输出轴圈数（来自编码器 ÷700，浮点）→ 出粮克重
 * 结果只供喂食目标判定/日志显示使用，无副作用
 */
static float Feed_GetTransWeight(float OutputRot)
{
    return OutputRot * GRAM_PER_OUTPUT_ROT;
}

ESP_EVENT_DEFINE_BASE(FEED_EVENTS);   /* 事件族实体定义（全工程唯一） */

static QueueHandle_t s_FdQueue;       /* 作业队列：入队决策通过后的作业（喂食任务专属） */
static QueueHandle_t s_FdReqQueue;    /* 投递队列：事件回调只投这里，喂食任务收后做决策 */
static QueueHandle_t s_IRQueue;       /* 红外消息队列：红外模块还没写，暂时放这里 */
                                      /*（模块成型后：队列由红外驱动创建并投递，喂食服务只订阅/接收） */

static const char *FD_TAG = "FEED";

static uint8_t s_bFdQueueOwnedImdtSource = 0;   /* 立即喂食标志：已有立即作业在飞 = 1（单写者） */

static FdState_t s_FdState = FEED_ST_READY;     /* 当前状态（初始就绪；NONE 仅作兜底） */

static FdData_t s_FdData;                       /* 当前作业缓存：就绪态取出，运行/收尾期间读取 */

/* 卡粮检测（运行态"该转没转"）：连续 FEED_NOPULSE_TICKS 个 tick 脉冲数不动即判卡。
 * 进入运行态（作业开始 / 自愈恢复）时由 Feed_ResetPulseMonitor() 复位基线。 */
static int32_t  s_LastPulse = -1;   /* 上个 tick 的脉冲数；-1 = 未建立基线（本作业第一拍） */
static uint8_t  s_NoPulseTick = 0;  /* 脉冲数未变化的连续 tick 计数 */

static int32_t s_RecoverLastPulse = -1;   /* 上个 tick 的脉冲数；-1 = 未建立基线（本作业第一拍） */

/* 前向声明：Feed_RunningHandler 先于定义用到，需先声明避免 C23 隐式声明报错 */
static uint8_t Feed_PulseStalled(int PulseCnt, int32_t* LastPulse);

/* 复位卡粮基线（转移层进入运行态时调用，防止旧值误触发） */
static void Feed_ResetPulseMonitor(void)
{
    s_LastPulse = -1;
    s_NoPulseTick = 0;
}

/*
 * FEED_REQUEST 事件回调（esp_event 任务上下文）
 * 只投递不判断：立即去重/拒绝决策在喂食任务里做（见 Feed_HandleRequest），
 * 保证标志位单写者，也避免事件任务里阻塞/长逻辑。
 * @note 队列满时 0 超时直接丢弃——本设计宁可丢请求也不阻塞事件循环。
 */
static void Feed_RequestHandler(void* pHandlerArgs, esp_event_base_t Base, int32_t Id, void* pEventData)
{
    xQueueSend(s_FdReqQueue, pEventData, 0);
}

/* 状态动作：就绪态（感知层）——只查作业队列：有则取出缓存到 s_FdData 并报"拿到"；空则驻留。
 * 取作业是查询式动作（取走即不重复），硬件启动不在此做，留给转移层进入运行态那一步。 */
static FdActRes_t Feed_ReadyHandler()
{
    if (xQueueReceive(s_FdQueue, &s_FdData, 0) == pdTRUE)
    {
        ESP_LOGI(FD_TAG, "Receive feed event source.");
        return FD_RES_JOB_TAKEN;
    }
    return FD_RES_JOB_NONE;
}

/*
 * 状态动作：运行态（感知层）——每 tick 读编码器脉冲，做两件事：
 *  ① 卡粮判定：该转没转 = 脉冲数连续 FEED_NOPULSE_TICKS 个 tick 不变 → FD_RES_NO_PULSE；
 *  ② 到达判定：输出圈数换算克重 ≥ 目标 → FD_RES_TARGET_REACHED。
 * 电机维持转动不需要这里操心：PWM 一发硬件持续转（转移层进入运行态时启动）。
 */
static FdActRes_t Feed_RunningHandler()
{
    /* 本 tick 脉冲快照：只读一次，卡粮判定与到达判定共用同一份，避免多次读数间计数器增长导致不一致 */
    int PulseCnt = Encoder_GetCount();

    /* ① 卡粮"该转没转"：Feed_PulseStalled 返回 1 = 连续 N tick 无脉冲（卡住） */
    if (Feed_PulseStalled(PulseCnt, &s_LastPulse))
    {
        ESP_LOGI(FD_TAG, "Jam detected (no pulse for %d ticks).", s_NoPulseTick);
        return FD_RES_NO_PULSE;                 /* → 异常态（反转自愈） */
    }

    /* ② 到达判定：用同一份快照换算输出圈数（注：注入假计数时此值同样走假读数） */
    float OutputRotCnt = Encoder_CountToOutputRot(PulseCnt);
    float TransWeight = Feed_GetTransWeight(OutputRotCnt);
    if (TransWeight >= s_FdData.Weight)
    {
        ESP_LOGI(FD_TAG, "Feed done: TransWeight = %.1f g (target %d g), OutputRot = %.3f, PulseCnt = %d.",
                 TransWeight, s_FdData.Weight, OutputRotCnt, PulseCnt);
        return FD_RES_TARGET_REACHED;
    }
    return FD_RES_IN_PROGRESS;
}

/*
 * 脉冲"卡住"检测：传入本 tick 快照 PulseCnt 与上次值指针 LastPulse，判断脉冲是否连续不动。
 * 语义：返回 1 = 卡住（该转没转，连续 FEED_NOPULSE_TICKS 个 tick 无变化）；0 = 正常。
 * 约定：*LastPulse < 0 表示未建立基线（进入新阶段第一拍），此处建立基线并清零无脉冲计数。
 */
static uint8_t Feed_PulseStalled(int PulseCnt, int32_t* LastPulse)
{
    if (*LastPulse < 0)
    {
        *LastPulse = PulseCnt;
        s_NoPulseTick = 0;                      /* 新阶段基线：清零，防上一阶段残留误判 */
    }
    else if (PulseCnt != *LastPulse)
    {
        *LastPulse = PulseCnt;
        s_NoPulseTick = 0;                      /* 有脉冲：正常，清卡住计数 */
    }
    else if (++s_NoPulseTick >= FEED_NOPULSE_TICKS)
    {
        return 1;                               /* 连续 N tick 无脉冲 = 卡住 */
    }
    return 0;
}

static FdActRes_t Feed_ErrorReverseHandler()
{
    /* 反转阶段 */
    /* 本 tick 快照：只读一次，卡住判定用同一份 */
    int PulseCnt = Encoder_GetCount();

    if (Feed_PulseStalled(PulseCnt, &s_RecoverLastPulse))
    {
        /* 反转过程卡住，尝试正转 */
        return FD_RES_REVERSE_STALLED;
    }
        
    if (Encoder_GetDeltaAngle(PulseCnt, s_LastPulse) <= RECOVER_REVERSE_ANGLE)
    {
        return FD_RES_REVERSE_DONE;
    }
    return FD_RES_IN_PROGRESS;
}

static FdActRes_t Feed_ErrorForwardHandler()
{
    /* 本 tick 快照：只读一次，卡住判定用同一份 */
    int PulseCnt = Encoder_GetCount();

    if (Feed_PulseStalled(PulseCnt, &s_RecoverLastPulse))
    {
        /* 回正后仍无脉冲 = 自愈失败，重试（转移层计数）；重新走一遍反转 */
        return FD_RES_RETRY_FAIL;
    }

    if (Encoder_GetDeltaAngle(PulseCnt, s_LastPulse) >= RECOVER_ANGLE)
    {
        /* 复位并正常运行一段距离 = 自愈成功 */
        return FD_RES_RETRY_OK;
    }
    return FD_RES_IN_PROGRESS;
}

/*
 * 状态动作：结束态（收尾感知）——检查收尾条件是否完成，完成即报 DONE。
 * 注意：清立即标志属"一次性收尾副作用"，但 END 态实际只驻留一 tick（报 DONE 即转就绪），
 *       放这里与放转移层效果等价；若将来收尾要跨多 tick 异步推进，需挪到转移层 END→READY。
 * TODO：失败置 chute_blocked 闸门 / 循环预约 re-arm / 发 FEED_END 事件（回灌发起方）
 */
static FdActRes_t Feed_EndHandler()
{
    if (s_FdData.Type == FEED_TYPE_IMMEDIATE)
    {
        s_bFdQueueOwnedImdtSource = 0;   /* 该立即作业已消费完，放行后续立即请求（单写者） */
    }
    return FD_RES_DONE;
}

/* 红外守卫动作：运行态收到堵粮消息时调用（当前占位；按 msg->type 分叉：出餐口堵→停机失败 / 卡粮→自愈） */
static void Feed_IR_RunningBlockHandler()
{
    
}

static void Feed_EnterRunningSt(void)
{
    Motor_Run();
    Feed_ResetPulseMonitor();           /* 卡粮判定基线清零，避免旧值误触发 */
}

static void Feed_EnterErrReverseSt(void)
{
    // 清自愈脉冲基线 + 启动反转
    s_RecoverLastPulse = -1;
    Motor_Recover_RunInverse();          /* 开始反转 */
}

static void Feed_EnterEndSt(void)
{
    Motor_Stop();
    Encoder_StopCount();
}

static void Feed_EnterErrForwardSt(void)
{
    s_RecoverLastPulse = -1;             /* 回正阶段重新建脉冲基线 */
    Motor_Run();                          /* 反转时间到，切回正转 */
}

/*
 * 状态转移（转移层，每 tick 集中评估一次）——双重职责：
 * ① 纯映射：结果码 → 下一状态。识别不出的结果一律驻留（FdState 初值 = 当前态，天然兜底）；
 *    重试计数 s_RetryCnt 归转移层管（动作层只管"这步自愈成没成"）。
 * ② 一次性副作用：每个"进入新状态"的分支内执行对应硬件开关 + 转移日志——
 *    只在切换发生的那一次执行（沿触发，与感知层每 tick 轮询互补），禁止放持续控制。
 * 自检清单（每个分支都要对上 mermaid 出边）：
 *    进 RUNNING（来自 READY/ERROR）→ 电机开/计数清+开；
 *    进 ERROR（来自 RUNNING/ERROR）→ 电机反转；
 *    进 END（来自 RUNNING/ERROR）→ 电机停/计数停；
 */
static FdState_t Feed_StateTransition(const FdActRes_t ActRes)
{
    static uint8_t s_RetryCnt = 0;
    FdState_t FdState = s_FdState;          /* 默认驻留：IN_PROGRESS/NONE/未知结果都原地待命 */
    if (ActRes == FD_RES_JOB_NONE)
    {
        FdState = FEED_ST_READY;
    }
    /* 就绪态：拿到资源 → 进入运行态（一次性：开电机 + 清计数 + 开始计数） */
    else if (ActRes == FD_RES_JOB_TAKEN)
    {
        ESP_LOGI(FD_TAG, "Enter FEED_ST_RUNNING state.");
        Encoder_ClearCount();               /* 新作业计数从 0 起算：不清会拿上次残留瞬间"到达" */
        Encoder_StartCount();

        Feed_EnterRunningSt();

        ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_START, &s_FdData, sizeof(s_FdData), 0));

        FdState = FEED_ST_RUNNING;
    }
    /* 运行态：转数/克重到达 → 结束态（一次性：停电机 + 停计数） */
    else if (ActRes == FD_RES_TARGET_REACHED)
    {
        ESP_LOGI(FD_TAG, "Enter FEED_ST_END state.");
        Feed_EnterEndSt();
        FdState = FEED_ST_END;
    }
    /* 运行态：该转没转 → 异常态（一次性：尝试反转自愈） */
    else if (ActRes == FD_RES_NO_PULSE)
    {
        ESP_LOGI(FD_TAG, "Enter FEED_ST_ERROR_REVERSE state.");

        Feed_EnterErrReverseSt();

        FdMsgData_t FdMsgData = {
            .FdData = s_FdData,
            .Msg = "Enter FEED_ST_ERROR_REVERSE state."
        };
        ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_BLOCK, &FdMsgData, sizeof(FdMsgData), 0));

        FdState = FEED_ST_ERROR_REVERSE;
    }
    /* 异常态：自愈成功 → 回运行态续跑（一次性：切回正转；不清计数 = 保留已完成的进度） */
    else if (ActRes == FD_RES_RETRY_OK)
    {
        ESP_LOGI(FD_TAG, "ERROR_RETRY is OK. Enter FEED_ST_RUNNING state.");
        s_RetryCnt = 0;
        s_RecoverLastPulse = -1;

        Feed_EnterRunningSt();

        ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_RECOVER, &s_FdData, sizeof(s_FdData), 0));

        FdState = FEED_ST_RUNNING;
    }
    /* 异常态：自愈失败 → 未达上限留异常 / 达上限结束收尾（一次性：出异常必停硬件） */
    else if (ActRes == FD_RES_RETRY_FAIL)
    {
        s_RetryCnt++;
        s_RecoverLastPulse = -1;
        if (s_RetryCnt >= FEED_ERROR_RETRY_TIMES)
        {
            ESP_LOGI(FD_TAG, "RetryCnt reach FEED_ERROR_RETRY_TIMES.");
            s_RetryCnt = 0;
            Feed_EnterEndSt();
            FdState = FEED_ST_END;
        }
        else
        {
            Feed_EnterErrReverseSt();
            FdState = FEED_ST_ERROR_REVERSE;
        }
    }
    /* 异常态：反转到位 / 自愈时阻塞 → 进入正转验证 */
    else if (ActRes == FD_RES_REVERSE_DONE || ActRes == FD_RES_REVERSE_STALLED)
    {
        Feed_EnterErrForwardSt();
        FdState = FEED_ST_ERROR_FORWARD;
    }
    /* 结束态：收尾完成 → 回就绪（一次性收尾副作用见 Feed_EndHandler 注释） */
    else if (ActRes == FD_RES_DONE)
    {
        ESP_LOGI(FD_TAG, "Enter FEED_ST_READY state.");
        // 结束阶段清理一次计数
        Encoder_ClearCount();
        
        ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_END, &s_FdData, sizeof(s_FdData), 0));

        FdState = FEED_ST_READY;
    }
    return FdState;
}

/*
 * 状态机"动作"分派器（调度职责）：按当前状态选执行感知层 handler，返回结果码。
 * 不做任何判断/副作用——判断在 handler 内部，副作用在转移层。
 * @note 非阻塞铁律：各 case 函数必须微秒级返回，不能内部阻塞等待
 */
static FdActRes_t Feed_SmAct(const FdState_t FdState)
{
    FdActRes_t FdActRes = FD_RES_NONE;
    switch (FdState)
    {
    case FEED_ST_NONE:
        break;
    case FEED_ST_READY:
        FdActRes = Feed_ReadyHandler();
        break;
    case FEED_ST_RUNNING:
        FdActRes = Feed_RunningHandler();
        break;
    case FEED_ST_ERROR_REVERSE:
        FdActRes = Feed_ErrorReverseHandler();
        break;
    case FEED_ST_ERROR_FORWARD:
        FdActRes = Feed_ErrorForwardHandler();
        break;
    case FEED_ST_END:
        FdActRes = Feed_EndHandler();
        break;
    default:
        break;
    }
    return FdActRes;
}

/*
 * 红外堵粮消息的守卫响应（喂食任务上下文，由 Feed_Run 收到消息后调用）
 * 按当前状态分派：运行→按消息类型转异常/结束；异常两态/就绪/结束 → 忽略消费（异常期不响应红外）；
 * 就绪/结束→不该有消息（残留），忽略（消费掉防污染下个作业）
 */
static void Feed_OnBlockage(const IR_Data_t* pIRData)
{
    switch (s_FdState)
    {
    case FEED_ST_RUNNING:
        ESP_LOGI(FD_TAG, "Handle FEED_ST_RUNNING state.");
        Feed_IR_RunningBlockHandler();
        break;
    case FEED_ST_NONE:
        ESP_LOGI(FD_TAG, "ERROR: Block in Unexpected FdState!");
        break;
    default:
        break;
    }
}

/* 创建全部队列并注册事件订阅。队列长度=5、按载荷字节数创建（队列存值拷贝）。 */
void Feed_Init(void)
{
    s_FdQueue = xQueueCreate(5, sizeof(FdData_t));
    s_FdReqQueue = xQueueCreate(5, sizeof(FdData_t));
    s_IRQueue = xQueueCreate(5, sizeof(IR_Data_t));
    assert(s_FdQueue && s_FdReqQueue && s_IRQueue);
    ESP_ERROR_CHECK(esp_event_handler_register(FEED_EVENTS, FEED_REQUEST, Feed_RequestHandler, NULL));
}

/*
 * 入队决策（只允许在喂食任务上下文调用）
 *  - 立即请求：查立即标志——已有立即作业在飞则拒绝（post FEED_REJECT 回灌发起方）；否则置位并入队
 *  - 预约请求：不查标志直接入队（立即只拦立即；预约与立即按队列先到先执行）
 *  - 只有"决策通过"的请求才会进入作业队列，被拒的永远不会被执行（消除幽灵作业）
 */
void Feed_HandleRequest(FdData_t* pFdData)
{
    if (!pFdData)
    {
        return;
    }

    if (pFdData->Type == FEED_TYPE_IMMEDIATE && s_bFdQueueOwnedImdtSource)
    {
        ESP_LOGI(FD_TAG, "Reject: There is immediate feed source.");
        FdMsgData_t FdMsgData = {
            .FdData = *pFdData,
            .Msg = "Reject: There is immediate feed source."
        };
        ESP_ERROR_CHECK(esp_event_post(FEED_EVENTS, FEED_REJECT, &FdMsgData, sizeof(FdMsgData), 0));
        return;
    }

    if (pFdData->Type == FEED_TYPE_IMMEDIATE)
    {
        s_bFdQueueOwnedImdtSource = 1;   /* 置位：此后新的立即请求将被拒绝（喂食任务单写者） */
    }
    xQueueSend(s_FdQueue, pFdData, 0);
}

/*
 * 喂食任务单 tick（由周期任务循环调用，如 while(1){ Feed_Run(); }）
 * 流水线：①红外守卫（10 tick 带超时，消息即打断）→ ②请求入队决策 → ③感知动作 → ④转移+一次性副作用
 * 其中 xQueueReceive 的 10 tick 超时 = 状态机 tick 周期（100ms @100Hz），期间让出 CPU
 */
void Feed_Run(void)
{
    IR_Data_t IRData;
    if (xQueueReceive(s_IRQueue, &IRData, 10) == pdTRUE)
    {
        /* ① 触发了红外守卫：立刻处理（堵粮响应要快，守卫例外不走 Act 链） */
        Feed_OnBlockage(&IRData);
    }

    FdData_t FdReq;
    while (xQueueReceive(s_FdReqQueue, &FdReq, 0) == pdTRUE)
    {
        /* ② 投递队列里的请求：喂食任务上下文中做入队决策（立即去重/拒绝回灌） */
        Feed_HandleRequest(&FdReq);
    }

    /* ③④ 状态机：感知动作（非阻塞切片）→ 转移（切换 + 进入新状态的一次性硬件副作用） */
    FdActRes_t ActRes = Feed_SmAct(s_FdState);
    s_FdState = Feed_StateTransition(ActRes);
}

/* 当前状态机状态（调试/测试用，只读） */
FdState_t Feed_GetState(void)
{
    return s_FdState;
}
