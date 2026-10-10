#include "MqttBackend.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "mqtt_client.h"
#include "esp_crt_bundle.h"
#include <stdlib.h>
#include <string.h>

/*
 * MqttBackend —— 实现（§11.8.1 / §11.8.2 / §11.8.3）
 *
 * 进度：① 事件打包点 = **纯转发**（已可用）② 建客户端 + start（已可用）
 *       ③ 后端任务的 SUB/UNSUB/PUB 执行、闸门、丢弃计数周期日志 → 仍是 TODO（B1~B14）。
 * ⚠️ 待办（B1 / §11.8.6）：档 B（NVS / 配网）还没落地 → 现在靠 `MB_USE_DEV_DEFAULT` 的编译期默认顶上；
 *    "缺 broker 配置 → client 停失能"要等档 B 接上后才生效。
 */

static const char *TAG = "MqttBackend";

/* ---------- 编译期常量（档 A，§11.8.6 / §11.8.7） ---------- */
#define MB_TASK_STACK           3072      /* §11.8.1：3 KB */
#define MB_TASK_PRIO            5
#define MB_DROP_LOG_PERIOD_MS   30000     /* 丢弃计数周期汇总日志（§11.8.3） */
#define MB_CONFIG_BROKER_URL    "mqtt://192.168.0.103:1883"   /* 开发期档 A 默认（见 MB_USE_DEV_DEFAULT）
                                                                * ⚠️ 这是本机 WLAN 的 DHCP 地址、会变（2026-10-06 实测从 .105 变成 .103）
                                                                * ⇒ 建议给 PC 做 DHCP 保留 / 静态 IP，否则每变一次都要改这里 */
#define MB_CONFIG_BUFF_SIZE     1024
#define MB_LOOP_TICK_MS         100       /* client 轮次心跳：MqttBackend_Loop() 取事件的超时（§十二 #12） */

/* TLS / 根证书包开关（**必须编译期**）：§11.8.7「认证/TLS 字段留位，v1 可不开」。
 * 置 1 → 链入 x509_crt_bundle（根证书清单），实测 app 从 ~852 KB 涨到 ~1.05 MB，
 *         1 MB 分区直接装不下（2026-10-06 实测）。
 * ⚠️ 不能用运行时 if (s_Cfg.UseTls) 代替：只要代码里出现对 esp_crt_bundle_attach 的
 *    **符号引用**，链接器就会把 x509_crt_bundle.S.obj 拖进来——运行时分支救不了体积。*/
#define MB_TLS_ENABLE           0

/* 开发期兜底（档 A，§11.8.6）：没有 NVS / SetConfig 来的配置时用编译期默认，
 * 免得一上电就拿 NULL 的 BrokerUri 去连。
 * ⚠️ 配网（档 B）落地后置 0，让"缺 broker 配置 → client 停失能"这条规则生效。*/
#define MB_USE_DEV_DEFAULT      1

/* ---------- 静态分配（§11.8.1 / §11.8.3） ---------- */
static QueueHandle_t            s_EvtQ      = NULL;   /* 后端 → client，20 条 */
static QueueHandle_t            s_ReqQ      = NULL;   /* client → 后端，16 条 */
static esp_mqtt_client_handle_t s_Client    = NULL;
static TaskHandle_t             s_Task      = NULL;
static volatile bool            s_Connected = false;  /* 平台事实镜像（§11.8.2） */
static MbConfig_t               s_Cfg;                /* 档 B 配置（TODO-B1） */

static uint32_t s_ReqDropped;      /* 请求丢弃计数（未连接 / 投递瞬间断线） */
static uint32_t s_PublishFail;     /* publish 失败计数（-1 参数/其他；-2 outbox 超限） */
static uint32_t s_EvtOverflow;     /* 事件信箱溢出计数 */
static uint32_t s_TopicDropped;    /* 主题不匹配丢弃计数（下行只接受 cmd） */

static OnMsgCallback s_OnMsg = NULL;
static char          s_CmdTopic[MB_TOPIC_MAX];   /* 下行唯一接受的主题（含设备 id，由 client 注册） */
static volatile bool s_HasCmdTopic = false;

/* ---------- 投递统一封装：回调只调它，不裸碰队列（§11.2） ---------- */
static void Mb_PostEvent(const MbEvent_t *Evt)
{
    if (xQueueSend(s_EvtQ, Evt, 0) != pdTRUE) 
    {
        s_EvtOverflow++;   /* 丢新 + 计数（§十 Q4）；周期汇总日志见 TODO-B14 */
    }
}

/* ---------- 事件打包点（§11.8.2）：纯转发 + 一个注册进来的主题过滤 ----------
 * 三铁律：只做「深拷贝 + 入队 + 唤醒」，不解析、不组包、不判断。
 * 唯一的"判断"是 DATA 的主题过滤（只放行 client 注册的那条 cmd 主题，见下）。
 * 事件 id 与错误结构直接透传库的类型 → **没有 per-event 的 switch**：
 * 库新增事件类型会自动流过；"我们实际只处理哪几种"归 client 的 switch（§11.8.5）。
 * 跑在 esp-mqtt 任务上下文；事件现场 data/topic 指针在回调返回后即失效 → 必须深拷贝。
 */
static void Mqtt_EventHandler(void *args, esp_event_base_t base, int32_t evt_id, void *evt_data)
{
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)evt_data;
    MbEvent_t out;

    (void)args;
    (void)base;

    /* ---- 1. 按值搬字段（不解释语义） ---- */
    out.Id              = (esp_mqtt_event_id_t)evt_id;
    out.MsgId           = e->msg_id;
    out.Topic           = e->topic;
    out.SessionPresent  = e->session_present;
    out.Retain          = e->retain;
    out.Dup             = e->dup;
    out.Qos             = e->qos;
    out.TotalDataLen    = e->total_data_len;
    out.DataOffset      = e->current_data_offset;
    out.Len             = 0;
    out.Payload[0]      = '\0';

    memset(&out.Err, 0, sizeof(out.Err));
    if (e->error_handle != NULL)
    {
        /* 整块按值拷。SUBACK 失败码也在里面：deliver_suback 先清 NONE
         * （mqtt_client.c:1418）再按需置 SUBSCRIBE_FAILED（:1424），故不会读到陈旧值。 */
        out.Err = *e->error_handle;
    }

    /* ---- 2. 唯一的分支：DATA 的载荷深拷贝（§11.8.2） ----
     * 回调返回后 e->data 指向的接收缓冲即被复用 → 必须拷走；
     * 超长 / 分片一律丢弃 + 告警（免得半截 JSON 在 client 侧变成 1001）。 */
    if (out.Id == MQTT_EVENT_DATA)
    {
        if (e->data == NULL)
        {
            ESP_LOGW(TAG, "DATA dropped: payload is NULL");
            return;
        }
        if (e->total_data_len != e->data_len || e->current_data_offset != 0)
        {
            ESP_LOGW(TAG, "DATA dropped: fragmented (len=%d total=%d offset=%d)",
                     e->data_len, e->total_data_len, e->current_data_offset);
            return;
        }
        if (e->data_len < 0 || e->data_len > MB_PAYLOAD_MAX)
        {
            ESP_LOGW(TAG, "DATA dropped: too long (%d, limit %d)", e->data_len, MB_PAYLOAD_MAX);
            return;
        }

        /* ---- 下行主题过滤（唯一一处"按主题分派"）----
         * 下行只接受 client 注册的那条 cmd 主题（含设备 id）。主题本身没有信息、
         * 只起分派作用 → 在这里匹配，既不必把 topic 装进条目（省 40 B/条），
         * 也让不匹配的报文连一个队列槽都不占。
         * ⚠️ 本段是 ≤36 B 的 memcmp，属"一次动作"，可以留在回调里（不许阻塞才是铁律）。 */
        if (e->topic == NULL || !s_HasCmdTopic
            || e->topic_len != (int)strlen(s_CmdTopic)
            || memcmp(e->topic, s_CmdTopic, (size_t)e->topic_len) != 0)
        {
            s_TopicDropped++;
            ESP_LOGW(TAG, "DATA dropped: unexpected topic '%.*s' (expect '%s')",
                     e->topic_len, e->topic != NULL ? e->topic : "(null)",
                     s_HasCmdTopic ? s_CmdTopic : "(unregistered)");
            return;
        }

        memcpy(out.Payload, e->data, (size_t)e->data_len);
        out.Payload[e->data_len] = '\0';   /* 便于 client 侧直接喂 cJSON */
        out.Len = e->data_len;
    }

    /* ---- 3. 唯一的副作用：is_connected 平台事实镜像（§11.8.2） ----
     * 必须落在回调里：请求闸门在后端任务、事件消费者 client 更慢，两边都来不及更新它。 */
    if (out.Id == MQTT_EVENT_CONNECTED)
    {
        s_Connected = true;
    }
    else if (out.Id == MQTT_EVENT_DISCONNECTED)
    {
        s_Connected = false;
    }

    Mb_PostEvent(&out);
}

/* ---------- 事件消费 + 分派（由 client 任务经 MqttBackend_Loop() 驱动） ----------
 * ⚠️ 这里已经**不在 esp-mqtt 任务里**了：s_OnMsg 回调跑在 client 任务上下文。
 * 取事件**带超时**（既不是 0 也不是 portMAX_DELAY）：
 *   · 0              → 调用者变忙等自旋（Client_Task 是 for(;;) 且无延时）
 *   · portMAX_DELAY  → 没有事件就永远不来一轮，"每轮末尾"的 flush / 订阅自愈 /
 *                      将来的状态机 tick 全部没有触发源（§十二 #12）
 *   取 100 ms 折中：无事件也周期性返回一轮。 */
static void Mqtt_EventHandler_(void)
{
    MbEvent_t Evt;

    if (xQueueReceive(s_EvtQ, &Evt, pdMS_TO_TICKS(MB_LOOP_TICK_MS)) != pdPASS)
    {
        return;
    }

    switch (Evt.Id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        /* ⚠️ 开发期临时做法：直接在这里订阅。
         * 正式路径 = client 状态机收到 CONNECTED → 投 MB_REQ_SUB → 后端任务执行（§11.8.4）。
         * 这里跑在 **client 任务**里，所以 esp_mqtt_client_subscribe 的同步 socket 写会占住它
         * （每次重连一次，正常几十微秒）——正式实现时搬回后端任务。*/
        if (s_HasCmdTopic && s_Client != NULL)
        {
            int sub_id = esp_mqtt_client_subscribe(s_Client, s_CmdTopic, 1);
            ESP_LOGI(TAG, "subscribe '%s' qos1 → msg_id=%d", s_CmdTopic, sub_id);
        }
        else
        {
            ESP_LOGW(TAG, "CONNECTED 但 cmd 主题未注册 → 不订阅");
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        /* 授予码 ≥ 0x80 被拒时，库把 error_type 置 SUBSCRIBE_FAILED 挂在**本事件**上
         * （deliver_suback 先清 NONE 再按需置，mqtt_client.c:1418/1424）*/
        if (Evt.Err.error_type == MQTT_ERROR_TYPE_SUBSCRIBE_FAILED)
        {
            ESP_LOGW(TAG, "MQTT_EVENT_SUBSCRIBED failed, msg_id=%d", Evt.MsgId);
        }
        else
        {
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", Evt.MsgId);
        }
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", Evt.MsgId);   /* v1 只作日志（§11.8.7）*/
        break;

    case MQTT_EVENT_DATA:
        /* 主题已在打包点过滤（只放行 cmd）→ 这里只把 payload 交给 client */
        if (s_OnMsg != NULL)
        {
            s_OnMsg(Evt.MsgId, Evt.Payload, Evt.Len);
        }
        else
        {
            ESP_LOGW(TAG, "DATA dropped: OnMsg 未注册");
        }
        break;

    case MQTT_EVENT_ERROR:
        if (Evt.Err.error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
        {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", Evt.Err.esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", Evt.Err.esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", Evt.Err.esp_transport_sock_errno,
                     strerror(Evt.Err.esp_transport_sock_errno));
        } 
        else if (Evt.Err.error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) 
        {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", Evt.Err.connect_return_code);
        } 
        else 
        {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", Evt.Err.error_type);
        }
        break;

    default:
        ESP_LOGI(TAG, "Other event id:%d", Evt.Id);
    }
}

/* ---------- 建客户端（§11.8.7：参数逐项显式写出，便于将来调） ---------- */
static esp_err_t Mqtt_CreateClient(void)
{
    /* TODO-B13 §11.8.7：把 s_Cfg 映射进 esp_mqtt_client_config_t 并 esp_mqtt_client_init()：
     *   broker.address.uri            = s_Cfg.BrokerUri
     *   credentials.client_id         = s_Cfg.ClientId
     *   credentials.username/password = s_Cfg.Username / s_Cfg.Password（可 NULL）
     *   session.keepalive             = 60
     *   session.disable_clean_session = false          → clean session = true（§二 09-21 定）
     *   session.protocol_ver          = MQTT_PROTOCOL_V_3_1_1
     *   session.last_will.topic/payload/msg/retain/qos → LWT = status/offline（retain）
     *   network.reconnect_timeout_ms  = 10000（= 库默认，即"内层 10s 自动重试"）
     *   buffer.size = 1024；outbox 4KB / 30s 过期；task.stack 6KB / prio 5（库默认，显式写出）
     *   credentials.authentication（TLS）字段留位，v1 不开
     * 然后 esp_mqtt_client_register_event(s_Client, ESP_EVENT_ANY_ID, Mqtt_EventHandler, NULL)。
     * ⚠️ ESPHome 式 reboot_timeout 不引入（§11.8.7 已否决）。 */
     /* ⚠️ 不能加 const：下面要按编译期开关有条件地填 verification 字段 */
     esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = s_Cfg.BrokerUri,
        },
        .session = {
            .keepalive = s_Cfg.KeepaliveSec,
            .disable_clean_session = false,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
            .last_will = {
                .topic = s_Cfg.WillTopic,
                .msg    = s_Cfg.WillPayload, // "{\"online\":false}"
                .msg_len = 0,  
                .qos = 1,
                .retain = true
            }
        },
        .credentials = {
            .client_id = s_Cfg.ClientId,
            .username = s_Cfg.Username,
            .authentication.password = s_Cfg.Password
        },
        .buffer.size = MB_CONFIG_BUFF_SIZE
    };

#if MB_TLS_ENABLE
    /* 只在开 TLS 时才挂根证书包（约 200 KB，理由见文件头 MB_TLS_ENABLE 注释）*/
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    ESP_LOGI(TAG, "esp client init.");
    s_Client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_Client == NULL)
    {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_Client, ESP_EVENT_ANY_ID, Mqtt_EventHandler, NULL));
    return ESP_OK;
}

/* ---------- 后端任务（§11.8.3）：全工程唯一执行 esp_mqtt_client_* 的地方 ---------- */
static void MqttBackend_Task(void *arg)
{
    MbReq_t req;

    (void)arg;
    // 中途掉线会丢信箱中的内容，如果是事件类，会放置到mqtt信箱中
    while (xQueueReceive(s_ReqQ, &req, portMAX_DELAY) == pdTRUE)
    {
        if (!s_Connected)
        {
            if (!req.Cacheable)
            {
                /* 非事件类（ack / err / boot / status / SUB）：没连上就没价值
                 * → 丢掉，顺手把队列槽腾出来 */
                s_ReqDropped++;
                continue;
            }
            /* 事件类：**不丢**，照样走下面的 publish。
             * QoS1 会进库的 outbox，连接恢复后由传输层自动补发
             * （窗口 = CONFIG_MQTT_OUTBOX_EXPIRED_TIMEOUT_MS）*/
        }

        switch (req.Type)
        {
        case MB_REQ_SUB:
            esp_mqtt_client_subscribe(s_Client, req.Topic, req.Qos);
            break;

        case MB_REQ_UNSUB:
            esp_mqtt_client_unsubscribe(s_Client, req.Topic);
            break;

        case MB_REQ_PUB:
        {
            int id = esp_mqtt_client_publish(s_Client, req.Topic, req.Payload,
                                             (int)req.Len, req.Qos, req.Retain);
            if (id < 0)
            {
                /* -1 = 参数/其他失败；-2 = outbox 超限。
                 * v1：只计数 + 周期告警（不重投）*/
                s_PublishFail++;
            }
            break;
        }

        default:
            break;
        }
    }
}

/* ================= 对外 API ================= */

void MqttBackend_Init(void)
{
    s_EvtQ = xQueueCreate(MB_EVT_Q_LEN, sizeof(MbEvent_t));
    s_ReqQ = xQueueCreate(MB_REQ_Q_LEN, sizeof(MbReq_t));
    if (s_EvtQ == NULL || s_ReqQ == NULL) 
    {
        ESP_LOGE(TAG, "mailbox create failed");
        return;                       /* 初始化失败不做运行期降级 */
    }
    if (xTaskCreate(MqttBackend_Task, "mqtt_backend", MB_TASK_STACK, NULL, MB_TASK_PRIO, &s_Task) != pdPASS) 
    {
        ESP_LOGE(TAG, "backend task create failed");
        return;
    }
#if MB_USE_DEV_DEFAULT
    /* 开发期档 A 兜底（§11.8.6）：没人 SetConfig 就用编译期默认，否则 BrokerUri 是 NULL。
     * ⚠️ 已知顺序缺口：档 B（NVS/配网）落地时 SetConfig 会晚于这里 → 届时要拆成
     *    Init(信箱+任务) + Start(建客户端+start)，或者让 client 在 Init 之前填好配置。*/
    if (s_Cfg.BrokerUri == NULL)
    {
        static const MbConfig_t DevCfg = {
            .BrokerUri    = MB_CONFIG_BROKER_URL,
            .ClientId     = NULL,       /* NULL → 库自生成 ESP32_xxxx（配网章再改成 {dev}）*/
            .Username     = NULL,
            .Password     = NULL,
            .WillTopic    = NULL,       /* 开发期先不要 LWT（§二 的 status/LWT 等 client 给主题）*/
            .WillPayload  = NULL,
            .KeepaliveSec = 60,         /* §11.8.7 */
            .ReconnectMs  = 10000,      /* = 库默认 */
            .UseTls       = false,      /* v1 明文（内网 / 隧道内已加密）*/
        };
        s_Cfg = DevCfg;
        ESP_LOGW(TAG, "using dev default broker: %s", MB_CONFIG_BROKER_URL);
    }
#endif

    /* TODO-B11 §九：首次 START 暂由 Init 代做（建客户端 + start）。
     * ⚠️ 这里绕过了 MQTT 状态机（§九 状态机尚未实现）→ 现在一上电就会去连 broker。 */
    ESP_ERROR_CHECK(Mqtt_CreateClient());

    /* 注：第三个任务由 esp-mqtt 库在 start 时自建（§11.8.1：esp-mqtt / 后端 / client 三任务） */
    /* TODO-B14 §11.8.3：起一个周期汇总日志（MB_DROP_LOG_PERIOD_MS），
     *   形态抄 ESPHome get_and_reset_dropped_count()：读并清零 s_ReqDropped / s_EvtOverflow。 */
    // 此处不应为空，为空需要终止
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_Client));
}

void MqttBackend_SetOnMsg(OnMsgCallback OnMsg, const char *CmdTopic)
{
    s_OnMsg = OnMsg;

    if (CmdTopic != NULL && strlen(CmdTopic) < MB_TOPIC_MAX)
    {
        /* 抄一份：调用方传进来的可能是可复用/局部缓冲（别依赖"它会一直活着"）*/
        size_t n = strlen(CmdTopic);
        memcpy(s_CmdTopic, CmdTopic, n + 1);
        s_HasCmdTopic = true;
    }
    else
    {
        s_HasCmdTopic = false;      /* 未注册主题 → 下行 DATA 一律丢弃并计数 */
    }
}

void MqttBackend_Loop(void)
{
    Mqtt_EventHandler_();
}

void MqttBackend_SetConfig(const MbConfig_t *Cfg)
{
    if (Cfg == NULL) 
    {
        return;
    }
    s_Cfg = *Cfg;   /* TODO-B1：只浅拷贝；字符串常驻由调用方保证（见头文件注） */
}

bool MqttBackend_IsConnected(void)
{
    return s_Connected;
}

bool MqttBackend_PostReq(const MbReq_t *Req)
{
    if (Req == NULL || s_ReqQ == NULL) 
    {
        return false;
    }
    if (xQueueSend(s_ReqQ, Req, 0) != pdTRUE) 
    {   /* 满 → 丢新 + 计数（§11.8.1） */
        s_ReqDropped++;
        return false;
    }
    return true;
}
