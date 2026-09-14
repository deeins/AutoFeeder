#ifndef __I2C_H
#define __I2C_H
#include "driver/gpio.h"
#include "driver/i2c_types.h"

// 宏传入函数，那么内部多次引用时，会多次执行，所以要只调用一次，就得缓存返回值
// 宏要和函数调用逻辑差不多，整体需要是一个语句，
// 假设是块的话，那么只要在调用时加一个分号结尾，
// 就会多一个空语句，导致一些逻辑错误，比如if后接宏，
// 宏本身是块的话，再来个分号空语句，就可能导致else找不到if
/*
if (cond)
    DS_ERR_CHECK(foo());   // 展开: if (cond) { ... };   ← 这个分号 = 一条空语句
else
    bar();
*/
#define MYESP_ERR_CHECK(err) \
do \
{ \
    esp_err_t _err_code = err; \
    if (_err_code != ESP_OK) \
    { \
        return _err_code; \
    } \
} while(0) \

// I2C设备通用注册，提供I2C读写接口和日志TAG
#define I2C_DEVICE_GENERIC_REGISTER(x) \
static i2c_master_dev_handle_t s_I2C_##x##_DevHandler = NULL; \
static const char* x##_TAG = #x; \
 \
static esp_err_t x##_ReadReg(uint8_t Address, uint8_t *Data, size_t Len) \
{ \
    esp_err_t err_code = I2C_RegisterRead(s_I2C_##x##_DevHandler, Address, Data, Len); \
    if (err_code != ESP_OK) \
    { \
        ESP_LOGI(x##_TAG, "ERROR: %s. Fail to Read data from " #x ".", esp_err_to_name(err_code)); \
        return err_code; \
    } \
    return ESP_OK; \
} \
 \
static esp_err_t x##_WriteReg(uint8_t Address, uint8_t Data) \
{ \
    esp_err_t err_code = I2C_RegisterWriteByte(s_I2C_##x##_DevHandler, Address, Data); \
    if (err_code != ESP_OK) \
    { \
        ESP_LOGI(x##_TAG, "ERROR: %s. Fail to write byte to " #x ".", esp_err_to_name(err_code)); \
        return err_code; \
    } \
    return ESP_OK; \
} \
 \
static esp_err_t x##_WriteRegs(uint8_t Address, const uint8_t *Data, size_t Len) \
{ \
    esp_err_t err_code = I2C_RegisterWrite(s_I2C_##x##_DevHandler, Address, Data, Len); \
    if (err_code != ESP_OK) \
    { \
        ESP_LOGI(x##_TAG, "ERROR: %s. Fail to write block to " #x ".", esp_err_to_name(err_code)); \
        return err_code; \
    } \
    return ESP_OK; \
}

void I2C_BusInit(gpio_num_t SclPin, gpio_num_t SdaPin);

void I2C_DeviceRegister(uint16_t DeviceAddress, i2c_master_dev_handle_t *dev_handle);

esp_err_t I2C_RegisterRead(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len);

esp_err_t I2C_RegisterWriteByte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data);

esp_err_t I2C_RegisterWrite(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, const uint8_t *data, size_t len);

/* 流式器件整帧写（如 SSD1306：无寄存器地址概念，帧首自带控制字节）：长度不受 I2C_WRITE_BUF_MAX_LEN 限制 */
esp_err_t I2C_DeviceWriteRaw(i2c_master_dev_handle_t dev_handle, const uint8_t *Data, size_t Len);

#endif
