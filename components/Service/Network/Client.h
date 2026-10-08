#ifndef __CLIENT_H
#define __CLIENT_H

/*
 * Client.h —— 网络模块「契约头」（§11.1 / §11.5）
 *   业务模块只 include 本头；Client 侧零 include 业务模块
 *   （解耦靠「下行注册表 + 上行直推」，见 §11.5 / §11.6）。
 *   设计文档：模块设计/服务/网络模块.md §零（一页纸）/ §11.5
 *
 *   ⚠️ 本头引入 cJSON → 调用 Net_RegisterDown/Net_RegisterStatus 的组件
 *      需在自己的 CMakeLists 里加 `json`（espressif/cjson）。
 */

#include "esp_event.h"
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>

/* ---------- 编译期常量（档 A，§11.8.6） ---------- */
#define NET_PROTO_V     1       /* 协议版本，从 1 起；不认识 → err 1003（§三） */
#define NET_ID_MAX      16      /* 信封 id 上限，超限 → err 1002（§11.3） */

/* ---------- 内部事件 Source 署名（§一） ---------- */
/* 业务模块 post 内部事件时填 Source = NET_SRC_MQTT；
 * 上行按来源过滤：拒答类只回给下指令的人（§五「上报过滤规则」）。 */
ESP_EVENT_DECLARE_BASE(NET_EVENTS);
#define NET_SRC_MQTT    NET_EVENTS

/* ---------- 协议层错误码（§六） ---------- */
typedef enum {
    NET_ERR_PARSE           = 1001,   /* d 非法 JSON / 非对象 */
    NET_ERR_BAD_ENVELOPE    = 1002,   /* 缺 v/id/t 或类型不符（含 id 超长） */
    NET_ERR_VERSION         = 1003,   /* v 非 1 */
    NET_ERR_UNKNOWN_CMD     = 1004,   /* t 不在下行注册表 */
    NET_ERR_BAD_PARAMS      = 1005,   /* 参数缺失/类型/范围错（由回调返回） */
    NET_ERR_BUSY            = 1006,   /* 预留：内部队列满 / 限流 */
} NetErrCode_t;

/* ---------- 协议适配契约（§11.5） ---------- */

/* 下行解包：模块解析 d、校验、post 内部事件；返回 0 或 NetErrCode_t。
 * Ref = 原消息 id（回指用，可空）。 */
typedef int  (*NetParseFn)(const cJSON *D, const char *Ref);

/* status 补字段：往 client 自建的 DRoot 里加本模块字段。
 * 字段名归各模块（如 time_valid），client 不认业务键（§11.5）。 */
typedef void (*NetStatusFn)(cJSON *DRoot);

/* ---------- 对外 API（§11.5） ---------- */

/* 注册（静态数组 → 业务模块 init 顺序自由，§11.1） */
void Net_RegisterDown  (const char *Cmd, NetParseFn Fn);
void Net_RegisterStatus(NetStatusFn Fn);

/* 上行直推：模块给消息名 + d（JSON 串），client 补 v/id/ts 并发布；
 * 离线且 cacheable（仅事件类）→ 进 TX 缓存（§11.4）。 */
void Client_Publish    (const char *Name, const char *D);

/* 由 NetInit 按序调用（§11.1） */
void Client_Init(void);

#endif /* __CLIENT_H */
