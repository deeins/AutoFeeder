#ifndef __I2C_H
#define __I2C_H
#include "driver/gpio.h"
#include "driver/i2c_types.h"

void I2C_BusInit(gpio_num_t SclPin, gpio_num_t SdaPin);

void I2C_DeviceRegister(uint16_t DeviceAddress, i2c_master_dev_handle_t *dev_handle);

esp_err_t I2C_RegisterRead(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len);

esp_err_t I2C_RegisterWriteByte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data);

esp_err_t I2C_RegisterWrite(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, const uint8_t *data, size_t len);

#endif
