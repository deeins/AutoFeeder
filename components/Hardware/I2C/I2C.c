#include "I2C.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "hal/i2c_types.h"
#include "soc/clk_tree_defs.h"
#include "soc/gpio_num.h"
#include <string.h>

#define CONFIG_SCL_SPEED_HZ         400000
#define I2C_MASTER_TIMEOUT_MS       1000
#define I2C_WRITE_BUF_MAX_LEN       16     /* 单次写事务数据上限（寄存器块写场景 ≤16） */

static i2c_master_bus_handle_t s_I2C_BusHandle = NULL;

/* 引脚(开漏/上拉)由 i2c_new_master_bus 内部完成配置，此处不重复配置 */
void I2C_BusInit(gpio_num_t SclPin, gpio_num_t SdaPin)
{
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .sda_io_num = SdaPin,
        .scl_io_num = SclPin,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = false    /* DS3231 模块自带外部上拉 */
    };

    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_cfg, &s_I2C_BusHandle));
}

void I2C_DeviceRegister(uint16_t DeviceAddress, i2c_master_dev_handle_t *dev_handle)
{
    i2c_device_config_t i2c_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DeviceAddress,
        .scl_speed_hz = CONFIG_SCL_SPEED_HZ,
    };

    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_I2C_BusHandle, &i2c_cfg, dev_handle));
}

esp_err_t I2C_RegisterRead(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(dev_handle, &reg_addr, 1, data, len, I2C_MASTER_TIMEOUT_MS);
}

esp_err_t I2C_RegisterWriteByte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_master_transmit(dev_handle, write_buf, sizeof(write_buf), I2C_MASTER_TIMEOUT_MS);
}

esp_err_t I2C_RegisterWrite(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, const uint8_t *data, size_t len)
{
    if (len >= I2C_WRITE_BUF_MAX_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t write_buf[1 + I2C_WRITE_BUF_MAX_LEN];
    write_buf[0] = reg_addr;
    memcpy(write_buf + 1, data, len);

    return i2c_master_transmit(dev_handle, write_buf, 1 + len, I2C_MASTER_TIMEOUT_MS);
}
