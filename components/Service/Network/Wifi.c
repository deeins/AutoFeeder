#include "Wifi.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_event_base.h"
#include "esp_log.h"
#include "esp_netif_types.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_wifi_types_generic.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "portmacro.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WIFI_NS             "wifi_cred"
#define KEY_CRED            "ssid"
#define BACKOFF_SERIES_LEN  (sizeof(s_BackoffSeries) / sizeof(s_BackoffSeries[0]))

const char* TAG = "WIFI";

ESP_EVENT_DEFINE_BASE(MYWIFI_EVENTS);

static esp_event_handler_instance_t s_WifiEvtInst = NULL;
static esp_event_handler_instance_t s_IpEvtInst = NULL;

static TaskHandle_t s_TaskHandle = NULL;

static uint8_t s_HasNvsCred = 0;
static WifiCred_t s_NvsCred;

static portMUX_TYPE s_CtrlMux = portMUX_INITIALIZER_UNLOCKED;

static WifiCtrl_t s_Ctrl;

static int64_t s_BackoffDeadline = 0;

static uint8_t s_RetryTimes = 0;
static const uint64_t s_BackoffSeries[] = { 1000000, 2000000, 4000000, 8000000, 16000000, 30000000 };

static uint8_t Wifi_IsCredValid(const WifiCred_t *Cred)
{
    return Cred !=NULL && 
           Cred->SSID_len >= 1 &&
           Cred->SSID_len <= SSID_MAX_LEN &&
           (Cred->Pass_len == 0 ||
           (Cred->Pass_len >= 8 &&
           Cred->Pass_len <= PASSWORD_MAX_LEN)
           );
}

static esp_err_t Wifi_GetNvsCred(WifiCred_t *Cred)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READONLY, &h) != ESP_OK)
    {
        return ESP_FAIL;
    }
    WifiCred_t c;
    size_t len = sizeof(c);
    esp_err_t err = nvs_get_blob(h, KEY_CRED, &c, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(WifiCred_t) || !Wifi_IsCredValid(&c))
    {
        Cred->SSID_len = 0;
        Cred->Pass_len = 0;
        return ESP_ERR_INVALID_SIZE;        /* 短 blob / 旧格式 / 内容不合法 → 一律当"无凭据" */
    }
    *Cred = c;
    return err;
}

static esp_err_t Wifi_SetNvsCred(const WifiCred_t *Cred)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_set_blob(h, KEY_CRED, Cred, sizeof(WifiCred_t));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Fail to set key %s.", KEY_CRED);
    }
    esp_err_t commit_err = nvs_commit(h);
    if (commit_err != ESP_OK)
    {
        err = commit_err;
        ESP_LOGW(TAG, "Fail to commit nvs data.");
    }
    nvs_close(h);
    return err;
}

static CredSrc_t Wifi_PickCred(const WifiCtrl_t *Ctrl, WifiCred_t *Out)
{
    if (Ctrl->HasCandidate)
    {
        *Out = Ctrl->Candidate;
        return CRED_SRC_CANDIDATE;
    }

    if (s_HasNvsCred)
    {
        *Out = s_NvsCred;
        return CRED_SRC_STORED;
    }

    return CRED_SRC_NONE;
}

// 失能态受理意图改变信号
static WifiActRes_t Wifi_DisableHandler(uint32_t Kind, const WifiCtrl_t *Ctrl)
{
    WifiCred_t cred;
    if (Ctrl->Intent == WIFI_INTENT_CONNECT && Wifi_PickCred(Ctrl, &cred) != CRED_SRC_NONE)
    {
        return WIFI_RES_ENABLE_CONNECT;
    }
    return WIFI_RES_IN_PROGRESS;
}

// 正在连接态只接受连接信号和失连信号
static WifiActRes_t Wifi_ConnectingHandler(uint32_t Kind, const WifiCtrl_t *Ctrl)
{
    if (Kind == WIFI_NTF_ESP_CONNECTED)
    {
        return WIFI_RES_GOT_IP;
    }
    else if (Kind == WIFI_NTF_ESP_DISCONNECT)
    {
        // 凭据类错误，密码/安全模式不匹配，不再重试
        if (Ctrl->LastDiscReason == WIFI_REASON_AUTH_FAIL ||
            Ctrl->LastDiscReason == WIFI_REASON_HANDSHAKE_TIMEOUT||
            Ctrl->LastDiscReason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT||
            Ctrl->LastDiscReason == WIFI_REASON_MIC_FAILURE||
            Ctrl->LastDiscReason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
        )
        {
            return WIFI_RES_CRED_FAIL;
        }
        return WIFI_RES_CONNECT_ERR;
    }
    return WIFI_RES_IN_PROGRESS;
}

// 已连接态只受理失连信号
static WifiActRes_t Wifi_ConnectedHandler(uint32_t Kind, const WifiCtrl_t *Ctrl)
{
    if (Kind == WIFI_NTF_ESP_DISCONNECT)
    {
        return WIFI_RES_CONNECT_ERR;
    }
    return WIFI_RES_IN_PROGRESS;
}

// 连接异常态处理意图改变信号，退避在状态机入口管
static WifiActRes_t Wifi_ConnectErrHandler(uint32_t Kind, const WifiCtrl_t *Ctrl)
{
    if (Kind == WIFI_NTF_ESP_CONNECTED)
    {
        return WIFI_RES_GOT_IP;
    }
    if (Kind == WIFI_NTF_RETRY_DUE)
    {
        return WIFI_RES_RECONNECT;
    }
    return WIFI_RES_IN_PROGRESS;
}

static WifiActRes_t Wifi_SmAct(uint32_t Kind, const WifiCtrl_t *Ctrl)
{
    // 非失能态挂起意图，暂时使用于配网
    if (Kind == WIFI_NTF_CTRL_CHANGED && Ctrl->State != WIFI_ST_DISABLE && Ctrl->Intent == WIFI_INTENT_SUSPEND)
    {
        return WIFI_RES_DISABLE;
    }
    WifiActRes_t Res = WIFI_RES_IN_PROGRESS;
    switch (Ctrl->State) 
    {
    case WIFI_ST_DISABLE:
        Res = Wifi_DisableHandler(Kind, Ctrl);
        break;
    case WIFI_ST_CONNECTING:
        Res = Wifi_ConnectingHandler(Kind, Ctrl);
        break;
    case WIFI_ST_CONNECTED:
        Res = Wifi_ConnectedHandler(Kind, Ctrl);
        break;
    case WIFI_ST_CONNECT_ERR:
        Res = Wifi_ConnectErrHandler(Kind, Ctrl);
        break;
    default:
        break;
    }
    return Res;
}

static bool Wifi_TakeCandidate(WifiCred_t *Out)
{
    bool has;
    portENTER_CRITICAL(&s_CtrlMux);
    has = s_Ctrl.HasCandidate;
    if (has) {
        *Out = s_Ctrl.Candidate;
        memset(&s_Ctrl.Candidate, 0, sizeof s_Ctrl.Candidate);
        s_Ctrl.HasCandidate = 0;
    }
    portEXIT_CRITICAL(&s_CtrlMux);
    return has;                     /* 取的 = 清的，必然同源 */
}

static void Wifi_GiveBackCandidate(WifiCred_t *in)
{
    portENTER_CRITICAL(&s_CtrlMux);
    s_Ctrl.HasCandidate = 1;
    s_Ctrl.Candidate = *in;
    portEXIT_CRITICAL(&s_CtrlMux);
}

static void Wifi_ClearCandidate(void)
{
    portENTER_CRITICAL(&s_CtrlMux);
    s_Ctrl.Candidate.Pass_len = 0;
    s_Ctrl.Candidate.SSID_len = 0;
    s_Ctrl.HasCandidate = 0;
    portEXIT_CRITICAL(&s_CtrlMux);
}

static void Wifi_ConnectDataClear(void)
{
    s_RetryTimes = 0;
    portENTER_CRITICAL(&s_CtrlMux);
    s_Ctrl.LastDiscReason = WIFI_REASON_UNSPECIFIED;
    portEXIT_CRITICAL(&s_CtrlMux);
}

static void Wifi_BackoffTrans(WifiState_t *State)
{
    size_t idx = 0;
    if (s_RetryTimes >= BACKOFF_SERIES_LEN)
    {
        idx = BACKOFF_SERIES_LEN - 1;
    }
    else
    {
        idx = s_RetryTimes++;
    }
    // 设置退避时间
    s_BackoffDeadline = esp_timer_get_time() + s_BackoffSeries[idx];
    *State = WIFI_ST_CONNECT_ERR;
}

static void Wifi_TryToConnect(WifiState_t *State)
{
    esp_err_t connect_err = esp_wifi_connect();
    ESP_ERROR_CHECK_WITHOUT_ABORT(connect_err);

    if (connect_err == ESP_OK)
    {
        *State = WIFI_ST_CONNECTING;
    }
    else
    {
        // 有错误当成失败
        Wifi_BackoffTrans(State);
    }
}

static WifiState_t Wifi_StateTransition(WifiActRes_t Res, const WifiCtrl_t *Ctrl)
{
    WifiState_t State = Ctrl->State;
    if (Res == WIFI_RES_DISABLE)
    {
        State = WIFI_ST_DISABLE;
    }
    else if (Res == WIFI_RES_ENABLE_CONNECT)
    {
        WifiCred_t Cred;
        // 进了此分支默认存在凭据
        Wifi_PickCred(Ctrl, &Cred);
        Wifi_ConnectDataClear();
        wifi_config_t cfg;
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_get_config(WIFI_IF_STA, &cfg));
        // SSID和密码不能是全0，所以可以这么清空，SSID全0会被set_config拒绝
        memset(&cfg.sta.ssid, 0, sizeof(cfg.sta.ssid));
        memset(&cfg.sta.password, 0, sizeof(cfg.sta.password));
        memcpy(&cfg.sta.ssid, Cred.SSID, Cred.SSID_len);
        memcpy(&cfg.sta.password, Cred.Pass, Cred.Pass_len);
        // set前需要确保未连接，不然会报错
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &cfg));

        Wifi_TryToConnect(&State);
    }
    else if (Res == WIFI_RES_CRED_FAIL)
    {
        // 凭据有问题，需要重选凭据，走失能态逻辑
        // 理论上nvs里的凭据，正常不会走这里，
        // 因为nvs里的凭据是已验证过能连接才上传的
        // 但有可能会有改密码的情况，
        // 无候选走到这里就是改密码了
        if (Ctrl->HasCandidate)
        {
            Wifi_ClearCandidate();
            esp_event_post(MYWIFI_EVENTS, MYWIFI_EVENT_CANDIDATE_INVALID, NULL, 0, pdMS_TO_TICKS(10));
        }
        else
        {
            // 凭据失效，通过此方式标记凭据无效
            s_HasNvsCred = 0;
            // nvs凭据过期也走失能，因为没有比nvs数据更权威的了，没有其他可以尝试的选项
            esp_event_post(MYWIFI_EVENTS, MYWIFI_EVENT_NVS_CRED_EXPIRED, NULL, 0, pdMS_TO_TICKS(10));
        }
        State = WIFI_ST_DISABLE;
    }
    else if (Res == WIFI_RES_CONNECT_ERR)
    {
        Wifi_BackoffTrans(&State);
    }
    else if (Res == WIFI_RES_GOT_IP)
    {
        WifiCred_t used;
        if (Wifi_TakeCandidate(&used))                  /* 锁内取+清*/
        {
            esp_err_t err = Wifi_SetNvsCred(&used);
            if (err == ESP_OK || err == ESP_ERR_NVS_REMOVE_FAILED)   /* 后者=半成功，可用 */
            {
                s_NvsCred = used;
                s_HasNvsCred = 1;                 /* 镜像同步 */
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG, "commit half-done: %s", esp_err_to_name(err));
                }
            }
            else
            {
                Wifi_GiveBackCandidate(&used);                       /* 还回去，别丢数据 */
                ESP_LOGE(TAG, "commit failed: %s", esp_err_to_name(err));
                esp_event_post(MYWIFI_EVENTS, MYWIFI_EVENT_CRED_SAVE_FAILED,
                            NULL, 0, pdMS_TO_TICKS(10));
            }
        }
        s_RetryTimes = 0;
        State = WIFI_ST_CONNECTED;
    }
    else if (Res == WIFI_RES_RECONNECT)
    {
        Wifi_TryToConnect(&State);
    }
    return State;
}

// 状态机内部会建临界区修改s_Ctrl.State
static void Wifi_Run(uint32_t Kind, const WifiCtrl_t *Ctrl)
{
    WifiActRes_t Res = Wifi_SmAct(Kind, Ctrl);

    WifiState_t State = Wifi_StateTransition(Res, Ctrl);

    /* 跨入失能：停止连接活动（幂等；未连接时无操作、无事件）——必须在临界区外 */
    if (State == WIFI_ST_DISABLE && Ctrl->State != WIFI_ST_DISABLE)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
    }

    portENTER_CRITICAL(&s_CtrlMux);
    s_Ctrl.State = State;
    portEXIT_CRITICAL(&s_CtrlMux);
}

static TickType_t Wifi_WaitTicks(WifiState_t State)
{
    if (State != WIFI_ST_CONNECT_ERR)
    {
        return portMAX_DELAY;
    }

    // 异常退避
    int64_t left_us = s_BackoffDeadline - esp_timer_get_time();
    // 避免剩余时间为负，转为无符号数的时候，过大导致长时间等待
    if (left_us <= 0)
    {
        return 0;
    }
    // +9999避免提前到期
    return pdMS_TO_TICKS((left_us + 9999) / 1000);
}

static void Wifi_SnapshotCtrl(WifiCtrl_t *Ctrl)
{
    portENTER_CRITICAL(&s_CtrlMux);
    *Ctrl = s_Ctrl;                                 /* 整体快照，一致视图 */
    portEXIT_CRITICAL(&s_CtrlMux);
}

static void Wifi_RunTask(void* Parameter)
{
    while (1)
    {
        uint32_t kind = WIFI_NTF_NONE;
        WifiCtrl_t snapshot;
        BaseType_t got = xTaskNotifyWait(0, ULONG_MAX, &kind, Wifi_WaitTicks(s_Ctrl.State));
        Wifi_SnapshotCtrl(&snapshot);
        if (got != pdPASS)
        {
            kind = WIFI_NTF_RETRY_DUE;
        }
        Wifi_Run(kind, &snapshot);
    }
}

static void Wifi_EventHandler(void* event_handler_arg, 
                              esp_event_base_t event_base, 
                              int32_t event_id, 
                              void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e = event_data;

        /* ① 归因要用的：当场拷进寄存器（受锁保护） */
        portENTER_CRITICAL(&s_CtrlMux);
        s_Ctrl.LastDiscReason = e->reason;
        portEXIT_CRITICAL(&s_CtrlMux);

        /* ② 顺手留个日志：用 %.*s，别用 %s */
        ESP_LOGW(TAG, "disconnect: reason=%u rssi=%d ssid=%.*s",
                 e->reason, e->rssi, e->ssid_len, e->ssid);

        if (s_TaskHandle)
        {
            /* ③ 无脑转译成 kind（不判状态、不碰驱动） */
            xTaskNotify(s_TaskHandle, WIFI_NTF_ESP_DISCONNECT, eSetValueWithOverwrite);
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_TaskHandle)
        {
            /* ③ 无脑转译成 kind（不判状态、不碰驱动） */
            xTaskNotify(s_TaskHandle, WIFI_NTF_ESP_CONNECTED, eSetValueWithOverwrite);
        }
    }
}

void Wifi_Init(void)
{
    s_Ctrl.State = WIFI_ST_DISABLE;
    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL)
    {
        esp_log_level_set(TAG, CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    ESP_ERROR_CHECK(esp_netif_init());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &Wifi_EventHandler,
                                                        NULL,
                                                        &s_WifiEvtInst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &Wifi_EventHandler,
                                                        NULL,
                                                        &s_IpEvtInst));

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
            // H2E加密所需的密码标识符，一般不用
            .sae_h2e_identifier = "",
            // 扫描方式，fast是扫到第一个同名的就用
            .scan_method = WIFI_FAST_SCAN,
            // 通过RSSI排序，RSSI是接受到的WiFi信号强度
            .sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
            // 可接受的最小信号
            .threshold.rssi = 0,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    // 必须在set config前
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 初始化是否有凭据，获取nvs凭据
    esp_err_t err = Wifi_GetNvsCred(&s_NvsCred);
    if (err == ESP_OK && s_NvsCred.SSID_len > 0)
    {
        s_HasNvsCred = 1;
    }
    else
    {
        s_HasNvsCred = 0;
    }

    xTaskCreate(Wifi_RunTask, "Wifi_RunTask", 4096, NULL, 1, &s_TaskHandle);
    // 初始化意图，默认为尝试连接
    // 试连一次
    Wifi_SetIntent(WIFI_INTENT_CONNECT);
}

esp_err_t Wifi_SetCandidate(const WifiCred_t *cred)   /* 原语：任何任务可调 */
{
    /* ① 校验：参数 + 模块状态 —— 这是同步调用最大的收益，别浪费 */
    if (!Wifi_IsCredValid(cred))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_TaskHandle == NULL)                    /* init 前调用 */
    {
        return ESP_ERR_INVALID_STATE;
    }

    /* ② 临界区写共享状态：整份寄存器一次写完 */
    portENTER_CRITICAL(&s_CtrlMux);
    s_Ctrl.Candidate = *cred;                         /* 结构体整体赋值，锁内只有这一句 */
    s_Ctrl.HasCandidate = 1;
    portEXIT_CRITICAL(&s_CtrlMux);

    /* ③ 唤醒：必须在数据可见之后（出临界区就是屏障） */
    xTaskNotify(s_TaskHandle, WIFI_NTF_CTRL_CHANGED, eSetValueWithOverwrite);
    return ESP_OK;                               /* ④ 返回 ≠ 已连接 */
}

esp_err_t Wifi_SetIntent(WifiIntent_t Intent)
{
    if (s_TaskHandle == NULL)                    /* init 前调用 */
    {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_CtrlMux);
    s_Ctrl.Intent = Intent;
    portEXIT_CRITICAL(&s_CtrlMux);
    xTaskNotify(s_TaskHandle, WIFI_NTF_CTRL_CHANGED, eSetValueWithOverwrite);
    return ESP_OK;
}

esp_err_t Wifi_ClearNvsCred(void)
{
    nvs_handle_t h;
    if (nvs_open(WIFI_NS, NVS_READWRITE, &h) != ESP_OK)
    {
        return ESP_FAIL;
    }
    esp_err_t err = nvs_erase_key(h, KEY_CRED);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Fail to erase key %s.", KEY_CRED);
    }
    esp_err_t commit_err = nvs_commit(h);
    if (commit_err != ESP_OK)
    {
        err = commit_err;
        ESP_LOGW(TAG, "Fail to commit nvs data.");
    }
    nvs_close(h);
    return err;
}

uint8_t Wifi_IsConnected(void)
{
    uint8_t connected;
    portENTER_CRITICAL(&s_CtrlMux);
    connected = (s_Ctrl.State == WIFI_ST_CONNECTED);
    portEXIT_CRITICAL(&s_CtrlMux);
    return connected;
}
