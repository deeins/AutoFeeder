#include "NetInit.h"

#include "Client.h"
#include "MqttBackend.h"
#include "Wifi.h"

/*
 * NetInit —— 网络模块组装根（§11.1）
 *   平台层前置（由 app_main 负责，见 main/main.c）：
 *     nvs_flash_init + esp_event_loop_create_default（2026-10-03 定：NVS 归平台层）
 *   本函数按序：WiFi → MQTT 后端 → 客户端。
 */
void Network_Init(void)
{
    /* TODO-N0 §11.1：BLE 控制器初始化（配网期，P1 后置） */

    Wifi_Init();          /* 内含 esp_netif_init + esp_netif_create_default_wifi_sta */

    MqttBackend_Init();   /* 两个信箱 + 后端任务 */

    Client_Init();        /* {dev}/主题拼装 + 注册表 + client 任务 */

    /* 注册表用静态固定数组 → 业务模块 init 顺序自由（§11.1）：
     * 各业务模块的 Net_RegisterDown(...) 放在本函数之前或之后都可以。 */
}
