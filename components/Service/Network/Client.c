#include "Client.h"
#include "MqttBackend.h"

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Client —— 连接管理 + 协议适配（§11.1 / §11.5 / §11.8.5）
 *   · 自有任务：一次一条消费事件信箱；不阻塞；每轮末尾 flush 上行 + 订阅自愈
 *   · 依赖方向单向：本文件零 include 业务模块（下行注册 + 上行直推，§11.6）
 *
 * ⚠️ 本文件当前是【可编译骨架】：只有结构、签名与 TODO，**未实现功能**。
 *    TODO 编号在本文件内唯一（C0、C1…），每个 TODO 都指向设计章节。
 *    §十二 #10 / #12 是两个「写骨架时暴露的缺口」，对应 TODO-F1 / TODO-C8。
 */

static const char *TAG = "Client";

ESP_EVENT_DEFINE_BASE(NET_EVENTS);   /* NET_SRC_MQTT 的实体（§一） */

/* ---------- 编译期常量（档 A，§11.8.6） ---------- */
#define NET_DOWN_MAX        8        /* 下行注册表容量 */
#define NET_STATUS_MAX      4        /* status 回调容量 */
#define NET_ID_RING_LEN     16       /* 幂等环容量（§11.3） */
#define NET_TX_CACHE_LEN    16       /* TX 缓存条数（§十 Q5） */
#define NET_SUB_MAX         1        /* v1 只有 cmd 一条订阅（§11.8.4） */
#define NET_TASK_STACK      4096     /* §11.8.1：4 KB */
#define NET_TASK_PRIO       5
#define NET_BACKOFF_MS      5000     /* 未连接退避（§九评审修订：先照 ESPHome 三档） */
#define NET_DNS_TIMEOUT_MS  20000    /* 等待 DNS 应答上限 */
#define NET_CONN_TIMEOUT_MS 60000    /* 等待连接上限（内层 esp-mqtt 每 10s 自试） */
#define NET_SUB_HEAL_MS     5000     /* 订阅自愈重投间隔（§11.8.4） */

/* ---------- 主题（§11.8.4：init 时拼一次进静态缓冲，不落 NVS、不动态分配） ---------- */
static char s_DevId[7];                      /* {dev} = MAC 后 6 hex（§二） */
static char s_TopicCmd[MB_TOPIC_MAX];
static char s_TopicEvt[MB_TOPIC_MAX];
static char s_TopicStatus[MB_TOPIC_MAX];
static char s_TopicLog[MB_TOPIC_MAX];

/* ---------- 下行 / 状态注册表（§11.1：静态固定数组 → 注册不依赖任务已跑） ---------- */
typedef struct { const char *Cmd; NetParseFn Fn; } NetDownEntry_t;
static NetDownEntry_t   s_Down[NET_DOWN_MAX];
static uint8_t          s_DownCount;

typedef struct { NetStatusFn Fn; } NetStatusEntry_t;
static NetStatusEntry_t s_Status[NET_STATUS_MAX];
static uint8_t          s_StatusCount;

/* ---------- MQTT 状态机（§九 五态 + 退避） ---------- */
typedef enum {
    NET_ST_DISABLE = 0,      /* 失能：等外部开关 / 配网结果 */
    NET_ST_DISCONNECTED,     /* 未连接：退避计时 */
    NET_ST_DNS,              /* 等待 DNS 回复（≤20s） */
    NET_ST_CONNECTING,       /* 正在连接（≤60s） */
    NET_ST_CONNECTED,        /* 已连接（进入动作：重订阅 + 发 retained status/online） */
} ClientState_t;

typedef struct {
    ClientState_t State;
    uint8_t       RetryCount;      /* 计数用；退避间隔 = NET_BACKOFF_MS */
    int64_t       DeadlineUs;      /* 本状态截止时刻（esp_timer_get_time） */
    bool          SessionPresent;  /* 日志用（clean session = true 下恒 false，§二） */
} ClientCtx_t;

static ClientCtx_t s_Ctx;

/* ---------- 幂等环（§11.3）：容量 16、FIFO 覆盖最旧、存已处理 id ---------- */
typedef struct {
    char    Id[NET_ID_RING_LEN][NET_ID_MAX + 1];
    uint8_t Head;
    uint8_t Count;
} NetIdRing_t;

static NetIdRing_t s_IdRing;

/* ---------- TX 缓存（§11.2：client 内部，锁保护数组，不是队列） ---------- */
typedef struct {
    char     Topic[MB_TOPIC_MAX];
    char     Payload[MB_PAYLOAD_MAX];
    uint16_t Len;
    uint8_t  Qos;
    bool     Retain;
    bool     Cacheable;     /* 仅事件类可缓存；ack/err/boot/status 不缓存（§11.4） */
    bool     Used;
    uint32_t Ts;            /* 事件发生时刻（墙钟秒），非发布时刻 */
} NetTxItem_t;

static NetTxItem_t s_Tx[NET_TX_CACHE_LEN];

/* ---------- 订阅表（§11.8.4）：v1 只 cmd；将来加下行订阅只加一行 ---------- */
typedef struct {
    char    Topic[MB_TOPIC_MAX];
    uint8_t Qos;
    bool    Subscribed;
} SubEntry_t;

static SubEntry_t s_Subs[NET_SUB_MAX] = { { .Qos = 1 } };

/* ================= 注册表实现 ================= */

void Net_RegisterDown(const char *Cmd, NetParseFn Fn)
{
    if (Cmd == NULL || Fn == NULL || s_DownCount >= NET_DOWN_MAX) {
        ESP_LOGE(TAG, "Net_RegisterDown rejected: cmd=%s count=%u",
                 (Cmd != NULL) ? Cmd : "(null)", (unsigned)s_DownCount);
        return;
    }
    /* TODO-C0 待定：同 Cmd 重复注册 = 覆盖更新 还是 拒绝并告警？（建议覆盖 + 告警） */
    s_Down[s_DownCount].Cmd = Cmd;
    s_Down[s_DownCount].Fn  = Fn;
    s_DownCount++;
}

void Net_RegisterStatus(NetStatusFn Fn)
{
    if (Fn == NULL || s_StatusCount >= NET_STATUS_MAX) {
        ESP_LOGE(TAG, "Net_RegisterStatus rejected: count=%u", (unsigned)s_StatusCount);
        return;
    }
    s_Status[s_StatusCount].Fn = Fn;
    s_StatusCount++;
}

/* ================= 上行（§11.4 / §11.5） ================= */

void Client_Publish(const char *Name, const char *D)
{
    /* TODO-C1 §11.4/§11.5 —— 上行直推的完整落地：
     *   ① 组信封：{"v":NET_PROTO_V,"id":<新 id>,"t":Name,"d":<D>,"ts":<本地墙钟秒>}
     *      （ts 取"事件发生时刻"，即本函数被调用时刻；cJSON 组好 → PrintUnformatted）
     *   ② 判 cacheable：仅事件类（feed_* / time_*）可缓存；
     *      ack / err / boot / status → cacheable = false（§11.4）
     *   ③ 长度防御：超 MB_PAYLOAD_MAX → 告警并丢弃（协议上限）
     *   ④ 入 TX 缓存（溢出丢最旧）；并发保护用
     *      `static portMUX_TYPE s_TxLock = portMUX_INITIALIZER_UNLOCKED;`
     *      + taskENTER_CRITICAL（§11.2「锁保护数组」）
     *   ⑤ 不在本函数直接发：由 Client_FlushTx() 统一尝试（保证同轮 ack 同帧发出）
     *   ⑥ 消息名 Name 与 d 字段由各模块自行设定、与服务端共识（§11.5） */
    ESP_LOGW(TAG, "Client_Publish not implemented (TODO-C1): t=%s", (Name != NULL) ? Name : "(null)");
    (void)D;
}

/* 每轮末尾 flush（§11.8.5 ②） */
static void Client_FlushTx(void)
{
    /* TODO-C9 §11.4：
     *   · 闸门：MqttBackend_IsConnected()（§11.8.3 第一道闸）
     *   · 在线 → 每个 Used 条目组 MbReq_t{Type=MB_REQ_PUB, Qos, Retain, Len, Topic, Payload}
     *     投请求信箱；投成功则清 Used（失败 = 信箱满 → 保留待下轮）
     *   · 离线 → 什么都不做（cacheable 条目留在缓存，重连后按 ts 补发） */
}

/* 订阅自愈兜底（§11.8.4）：即使 CONNECTED 事件被丢弃也能爬起来 */
static void Client_SubHeal(void)
{
    /* TODO-C10 §11.8.4：
     *   IsConnected() && !s_Subs[0].Subscribed && (now - s_LastSubPostUs) > NET_SUB_HEAL_MS
     *   → 重投 MB_REQ_SUB（幂等），并刷新 s_LastSubPostUs */
}

/* 按指令名分派：**调用所有匹配的回调**（同名可注册多条，为将来多消费者留的）。
 * ⚠️ D = 信封里的 `d` 子对象、Ref = 信封的 `id`，由调用方解析好传进来；
 *    D 在回调返回后即被 cJSON_Delete 释放 → 回调必须当场取值，不能存指针。 */
static void Client_MatchCmd(const char *Cmd, const cJSON *D, const char *Ref)
{
    int     Err     = 0;
    uint8_t Matched = 0;

    for (uint8_t i = 0; i < s_DownCount; i++)
    {
        if (strcmp(Cmd, s_Down[i].Cmd) == 0)
        {
            Matched++;
            int e = s_Down[i].Fn(D, Ref);
            if (e != 0 && Err == 0)
            {
                Err = e;            /* 多回调时取第一个非 0 作为协议层错误码 */
            }
        }
    }

    if (Matched == 0)
    {
        Err = NET_ERR_UNKNOWN_CMD;      /* 1004：t 不在下行注册表里 */
    }

    /* TODO-C1 §11.5：Err != 0 → 组 err{code,msg,ref}；Err == 0 → 组 ack{ref}
     * （改型指令受理即回；status_get 以 status 应答代替 ack，见 §四），
     * 都入 TX 缓存，交给本轮末尾 Client_FlushTx() 发出。 */
    ESP_LOGI(TAG, "cmd '%s': matched=%u err=%d", Cmd, (unsigned)Matched, Err);
}

/* 下行 DATA 入口（由 MqttBackend_Loop() 在 **client 任务**里调用）。
 * · 主题已在后端打包点过滤（只放行 catfeeder/{dev}/cmd）→ 这里**不再判 topic**
 * · 上行类型（ack/err/…）不会出现在下行 cmd 主题上 → 也不判
 * · payload 已在打包点 NUL 结尾；长度用 PayloadLen（不依赖 strlen）*/
static void Client_OnMsg(int MsgId, char* Payload, int PayloadLen)
{
    cJSON *Root = cJSON_ParseWithLength(Payload, (size_t)PayloadLen);
    if (Root == NULL)
    {
        ESP_LOGW(TAG, "parse fail (msg_id=%d, len=%d)", MsgId, PayloadLen);
        /* TODO-C4 §11.3：回 err 1001（解析不出 id 时 ref 留空）*/
        return;
    }

    const cJSON *T  = cJSON_GetObjectItemCaseSensitive(Root, "t");
    const cJSON *D  = cJSON_GetObjectItemCaseSensitive(Root, "d");      /* ← 给回调的是 d */
    const cJSON *Id = cJSON_GetObjectItemCaseSensitive(Root, "id");     /* ← ref 的来源 */

    /* TODO-C4 §11.3：① 校验 v / id / t（1002/1003）② 幂等环查 id（命中只重发 ack/status）*/
    if (cJSON_IsString(T))
    {
        Client_MatchCmd(T->valuestring, D, cJSON_IsString(Id) ? Id->valuestring : "");
    }
    else
    {
        ESP_LOGW(TAG, "no 't' in payload");
        /* TODO-C4：回 err 1002 */
    }

    cJSON_Delete(Root);
}

/* ================= client 任务（§11.8.5） ================= */

static void Client_Task(void *arg)
{
    for (;;)
    {
        MqttBackend_Loop();

        /* ② 每轮末尾 flush 上行：保证本条指令的 ack 同轮发出（"同帧处理上下行"） */
        Client_FlushTx();

        /* ③ 订阅自愈兜底（§11.8.4） */
        Client_SubHeal();
    }
}

/* ================= 初始化（§11.1 / §11.8.4 / §11.8.6） ================= */

void Client_Init(void)
{
    memset(&s_Ctx, 0, sizeof(s_Ctx));
    memset(&s_IdRing, 0, sizeof(s_IdRing));
    memset(s_Tx, 0, sizeof(s_Tx));
    s_Ctx.State = NET_ST_DISABLE;      /* 缺 broker 配置 → 停失能（§11.8.6） */

    /* {dev}：TODO-C11 §二/§11.8.6 —— 现为占位，正式实现 = 读 MAC 后 6 hex（小写），
     * 首次生成后落 NVS 复用。⚠️ 占位值会让多台设备撞同一主题，真机联调前必须替换。 */
    snprintf(s_DevId, sizeof(s_DevId), "%s", "000000");

    /* 主题串：init 时拼一次进静态缓冲（§11.8.4） */
    snprintf(s_TopicCmd,    sizeof(s_TopicCmd),    "catfeeder/%s/cmd",    s_DevId);
    snprintf(s_TopicEvt,    sizeof(s_TopicEvt),    "catfeeder/%s/evt",    s_DevId);
    snprintf(s_TopicStatus, sizeof(s_TopicStatus), "catfeeder/%s/status", s_DevId);
    snprintf(s_TopicLog,    sizeof(s_TopicLog),    "catfeeder/%s/log",    s_DevId);
    memcpy(s_Subs[0].Topic, s_TopicCmd, sizeof(s_Subs[0].Topic));

    /* TODO-C12 §11.8.6 档 B：读 NVS 命名空间 `netcfg`（broker URI / 认证 / TLS / 时区）
     *   → MqttBackend_SetConfig(...)；开发期用"编译期默认 + NVS 镜像注入"替代配网
     *   （《WiFi模块自测清单-临时文件.md》附 A）。
     *   缺配置 → 保持失能 + 一条日志，不进连接状态机。 */

    /* 注册下行回调 + 下行唯一主题（含设备 id）→ 后端据此过滤，不匹配的不占队列槽 */
    MqttBackend_SetOnMsg(Client_OnMsg, s_TopicCmd);

    if (xTaskCreate(Client_Task, "mqtt_client", NET_TASK_STACK, NULL, NET_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "client task create failed");
    }
}
