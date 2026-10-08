# espressif/mqtt 与 espressif/cjson 由同目录 idf_component.yml 声明，
# 组件管理器会自动加入依赖（组件名 = espressif__mqtt / espressif__cjson），
# 因此这里不必（也不能）写成 mqtt / json。
# mbedtls：MqttBackend.c 用 esp_crt_bundle.h / esp_crt_bundle_attach（TLS 证书包）
idf_component_register(SRCS "Client.c" "MqttBackend.c" "NetInit.c" "Wifi.c"
                    PRIV_REQUIRES esp_wifi nvs_flash
                    INCLUDE_DIRS "."
                    REQUIRES esp_timer esp_event mbedtls
                    )
