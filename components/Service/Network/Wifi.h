#ifndef __WIFI_H
#define __WIFI_H

#include "esp_event_base.h"
#include "esp_err.h"
#include <stdint.h>

#define SSID_MAX_LEN        32
#define PASSWORD_MAX_LEN    64

ESP_EVENT_DECLARE_BASE(MYWIFI_EVENTS);

enum {
    MYWIFI_EVENT_CANDIDATE_INVALID,     /* 配网发来的候选密码/安全模式不匹配 */
    MYWIFI_EVENT_NVS_CRED_EXPIRED,      /* NVS内的凭据过期了，需要重新配网 */
    MYWIFI_EVENT_CRED_SAVE_FAILED,
};

typedef enum {
    WIFI_ST_DISABLE,
    WIFI_ST_CONNECTING,
    WIFI_ST_CONNECTED,
    WIFI_ST_CONNECT_ERR,
} WifiState_t;

typedef enum {
    WIFI_RES_IN_PROGRESS,
    WIFI_RES_DISABLE,
    WIFI_RES_ENABLE_CONNECT,
    WIFI_RES_CONNECT_ERR,
    WIFI_RES_GOT_IP,
    WIFI_RES_RECONNECT,
    WIFI_RES_CRED_FAIL,
} WifiActRes_t;

enum {
    WIFI_NTF_NONE = 0,          /* 0 保留：不可作 kind */
    WIFI_NTF_ESP_CONNECTED,     /* 1 边沿事实 GOT_IP        —— 必须独立 */
    WIFI_NTF_ESP_DISCONNECT,    /* 2 边沿事实 DISCONNECTED  —— 必须独立（含 reason） */
    WIFI_NTF_CTRL_CHANGED,      /* 3 控制面有更新：SET_INTENT / SET_CANDIDATE / */
    WIFI_NTF_RETRY_DUE,         /* 4 退避到期，开始重试 */
};

typedef enum {
    WIFI_INTENT_SUSPEND = 0,   /* 静止：不连、不重连（配网期 / 外部关闭 / 无凭据时的落点） */
    WIFI_INTENT_CONNECT = 1,   /* 持续意愿：连上并永续重试（用候选凭据，无候选则用 NVS 库） */
} WifiIntent_t;

typedef struct {
    unsigned char SSID[SSID_MAX_LEN];
    unsigned char Pass[PASSWORD_MAX_LEN];
    uint8_t SSID_len;
    uint8_t Pass_len;
} WifiCred_t;

// 临界区控制的数据
typedef struct {
    WifiCred_t   Candidate;
    WifiIntent_t Intent;        /* enum → 4 字节，要 4 字节对齐 → 有 2 字节 padding */
    uint8_t      HasCandidate;
    /* —— 上一次失连原因的输入（事件回调） —— */
    uint8_t      LastDiscReason;
    WifiState_t  State;
} WifiCtrl_t; 

typedef enum { 
    CRED_SRC_NONE, 
    CRED_SRC_CANDIDATE, 
    CRED_SRC_STORED 
} CredSrc_t;

void Wifi_Init(void);

esp_err_t Wifi_SetCandidate(const WifiCred_t *cred);

esp_err_t Wifi_SetIntent(WifiIntent_t Intent);

esp_err_t Wifi_ClearNvsCred(void);

uint8_t Wifi_IsConnected(void);

#endif
