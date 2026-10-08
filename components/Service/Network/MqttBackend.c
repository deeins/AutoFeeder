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
 * ⚠️ 待办（B1 / §11.8.6）：`s_Cfg` 目前无人填（`MqttBackend_SetConfig` 没被调用）→ `BrokerUri` 为 NULL；
 *    "缺 broker 配置 → client 停失能" 尚未落地，开发期应先用 `MB_CONFIG_BROKER_URL` 兜底。
 */

static const char *TAG = "MqttBackend";

/* ---------- 编译期常量（档 A，§11.8.6 / §11.8.7） ---------- */
#define MB_TASK_STACK           3072      /* §11.8.1：3 KB */
#define MB_TASK_PRIO            5
#define MB_DROP_LOG_PERIOD_MS   30000     /* 丢弃计数周期汇总日志（§11.8.3） */
#define MB_CONFIG_BROKER_URL    "mqtt://192.168.0.105:1883"
#define MB_CONFIG_BUFF_SIZE     1024

/* ---------- 静态分配（§11.8.1 / §11.8.3） ---------- */
static QueueHandle_t            s_EvtQ      = NULL;   /* 后端 → client，20 条 */
static QueueHandle_t            s_ReqQ      = NULL;   /* client → 后端，16 条 */
static esp_mqtt_client_handle_t s_Client    = NULL;
static TaskHandle_t             s_Task      = NULL;
static volatile bool            s_Connected = false;  /* 平台事实镜像（§11.8.2） */
static MbConfig_t               s_Cfg;                /* 档 B 配置（TODO-B1） */

static uint32_t s_ReqDropped;      /* 请求丢弃计数（未连接 / 投递瞬间断线） */
static uint32_t s_PublishFail;      /* 请求丢弃计数（未连接 / 投递瞬间断线） */
static uint32_t s_EvtOverflow;     /* 事件信箱溢出计数 */

/* ---------- 投递统一封装：回调只调它，不裸碰队列（§11.2） ---------- */
static void Mb_PostEvent(const MbEvent_t *Evt)
{
    if (xQueueSend(s_EvtQ, Evt, 0) != pdTRUE) 
    {
        s_EvtOverflow++;   /* 丢新 + 计数（§十 Q4）；周期汇总日志见 TODO-B14 */
    }
}

/* ---------- 事件打包点（§11.8.2）：**纯转发** ----------
 * 三铁律：只做「深拷贝 + 入队 + 唤醒」，不解析、不组包、不判断。
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

static void Mqtt_EventHandler_(void)
{
    MbEvent_t Evt;
    if (xQueueReceive(s_EvtQ, &Evt, 0) != pdPASS)
    {
        return;
    }

    switch (Evt.Id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED. topic = %s. ", Evt.Topic);
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED. topic = %s. ", Evt.Topic);
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED");
    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
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
     const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = s_Cfg.BrokerUri,
#if CONFIG_EXAMPLE_BROKER_CERTIFICATE_OVERRIDDEN
            .verification.certificate = cert_override_pem,
#elif CONFIG_EXAMPLE_CERT_VALIDATE_MOSQUITTO_CA
            .verification.certificate = (const char *)mosquitto_org_crt_start,
#else
            // 暂时先不管证书，用公用证书
            .verification.crt_bundle_attach = esp_crt_bundle_attach, /* Use built-in certificate bundle */
#endif
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
    /* TODO-B11 §九：首次 START 暂由 Init 代做（建客户端 + start）。
     * ⚠️ 这里绕过了 MQTT 状态机（§九 状态机尚未实现）→ 现在一上电就会去连 broker。 */
    ESP_ERROR_CHECK(Mqtt_CreateClient());

    /* 注：第三个任务由 esp-mqtt 库在 start 时自建（§11.8.1：esp-mqtt / 后端 / client 三任务） */
    /* TODO-B14 §11.8.3：起一个周期汇总日志（MB_DROP_LOG_PERIOD_MS），
     *   形态抄 ESPHome get_and_reset_dropped_count()：读并清零 s_ReqDropped / s_EvtOverflow。 */
    // 此处不应为空，为空需要终止
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_Client));
}

void MqttBackend_Loop(void)
{

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

QueueHandle_t MqttBackend_EventQueue(void)
{
    return s_EvtQ;
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
