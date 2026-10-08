#ifndef __MQTT_BACKEND_H
#define __MQTT_BACKEND_H

/*
 * MqttBackend —— esp-mqtt 薄封装 + 自有后端任务（§11.1 / §11.8.1~§11.8.3）
 *   · 全工程唯一调用 esp_mqtt_client_* 的地方
 *   · 严格黑盒：只搬 / 发 topic + payload 字符串，不解析（§11.1）
 *   · 自有任务：阻塞等请求信箱 → 执行 SUB/UNSUB/PUB；维护 is_connected 平台事实镜像
 *   设计文档：模块设计/服务/网络模块.md §11.8 / §11.8.7（参数取值）
 */

#include "esp_event.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------- 上限（编译期常量，§11.8） ---------- */
#define MB_TOPIC_MAX      36      /* "catfeeder/<6hex>/status" = 23，留余量 */
#define MB_PAYLOAD_MAX   256      /* 协议载荷上限（§十 Q4） */
#define MB_EVT_Q_LEN      20      /* 事件信箱：≥ 启动全量重推 burst 16 + 控制余量 */
#define MB_REQ_Q_LEN      16      /* 请求信箱：8~16（§11.8.1） */

/* ---------- 事件条目（后端 → client，§11.8.2） ----------
 * 事件 id 与错误结构**直接透传库的类型**（同 ESPHome `MQTTBackendESP32::Event`）：
 * 不自建枚举、不压扁错误码 → 少两个类型、少一层映射、不可能漂移（2026-10-06 定）。
 * 打包点是**纯转发**：把指针数据搬进自有缓冲（深拷贝）+ 把库字段按值拷进条目，
 * 不做任何解释——"订阅是否成功"之类由 client 读 `Err.error_type` 自己判。
 *
 * 字段类型照 esp_mqtt_event_t 原样，不窄化。
 * 刻意不搬：topic/topic_len（v1 唯一订阅 = cmd，指令名藏在 JSON 的 t 里）、
 *           client 句柄、protocol_ver、MQTT5 的 reason_code/property。
 */
typedef struct {
    esp_mqtt_event_id_t     Id;             /* = e->event_id，原样透传 */
    int                     MsgId;          /* SUBSCRIBED / PUBLISHED 对号 */
    const char*             Topic;
    int                     SessionPresent; /* CONNECTED */
    bool                    Retain;
    bool                    Dup;
    int                     Qos;
    int                     Len;                       /* DATA：本片有效长度（Payload 已 NUL 结尾） */
    int                     TotalDataLen;              /* DATA：整条消息总长（分片判定用） */
    int                     DataOffset;                /* DATA：本片偏移（分片判定用） */
    char                    Payload[MB_PAYLOAD_MAX + 1];   /* DATA：黑盒串深拷贝（+1 留结尾 NUL，
                                                            * 于是协议上限"≤256B"能被完整收下） */
    esp_mqtt_error_codes_t  Err;                       /* 整块按值拷；SUBACK 失败码就在 error_type 里 */
} MbEvent_t;

/* ---------- 请求条目（client → 后端，§11.8.3） ---------- */
typedef enum { MB_REQ_SUB, MB_REQ_UNSUB, MB_REQ_PUB, MB_REQ_START, MB_REQ_STOP } MbReqType_t;

typedef struct {
    MbReqType_t Type;
    uint8_t     Qos;
    uint8_t     Cacheable;
    bool        Retain;
    uint16_t    Len;
    char        Topic[MB_TOPIC_MAX];       /* 主题字符串随条目走 → 后端保持黑盒 */
    char        Payload[MB_PAYLOAD_MAX];   /* 仅 PUB 用 */
} MbReq_t;                                 /* ≈ 300 B */

/* ---------- 后端配置（档 B：由 NVS `netcfg` 填，§11.8.6） ----------
 * ⚠️ 字符串生命周期由调用方保证（静态/常驻缓冲），后端只存指针。 */
typedef struct {
    const char *BrokerUri;      /* "mqtt://host:1883" / "mqtts://…" */
    const char *ClientId;       /* {dev}，MAC 后 6 hex */
    const char *Username;       /* 可 NULL */
    const char *Password;       /* 可 NULL */
    const char *WillTopic;      /* status topic（retain + online=false） */
    const char *WillPayload;
    uint16_t    KeepaliveSec;   /* 60（§11.8.7） */
    uint16_t    ReconnectMs;    /* 10000 = 库默认 */
    bool        UseTls;         /* v1 false，字段留位 */
} MbConfig_t;

/* ---------- 对外 API ---------- */

void MqttBackend_Init(void);                        /* 建两信箱 + 起后端任务（由 NetInit 调用） */
void MqttBackend_Loop(void);
void MqttBackend_SetConfig(const MbConfig_t *Cfg);  /* 档 B 配置（§11.8.6） */
bool MqttBackend_IsConnected(void);                 /* 平台事实镜像（§11.8.2） */

QueueHandle_t MqttBackend_EventQueue(void);         /* client 任务从中取事件（后端创建） */
bool MqttBackend_PostReq(const MbReq_t *Req);       /* client → 请求信箱；满则 false */

#endif /* __MQTT_BACKEND_H */
