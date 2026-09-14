#include "DS3231.h"
#include "I2C.h"
#include "esp_err.h"
#include "esp_log.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define TIME_REG_ADDR           0x00
#define HOUR_REG_ADDR           0x02

#define CTRL_REG_ADDR           0x0E
#define STATUS_REG_ADDR         0x0F

#define OSF_BIT                 7
#define EOSC_BIT                7
#define A1IE_BIT                0
#define A2IE_BIT                1
#define AM_PM_SWITCH_BIT        6
#define EN32KHZ_BIT             3

I2C_DEVICE_GENERIC_REGISTER(DS3231)

// 首次上电或掉电时时间是否有效，0为有效，1为无效
static uint8_t s_OSF = 0;

static uint8_t DS3231_BCDToDec(uint8_t BCDNum)
{
    uint8_t Unit = 0b00001111 & BCDNum;
    uint8_t Hundr = (0b11110000 & BCDNum) >> 4;
    BCDNum = Hundr * 10 + Unit;
    return BCDNum;
}

static uint8_t DS3231_DecToBCD(uint8_t DecNum)
{
    return ((DecNum / 10) << 4) | (DecNum % 10);
}

static int DS3231_DecodeHour(uint8_t Raw)
{
    int IsPM = 0;

    if (Raw & (1 << AM_PM_SWITCH_BIT))
    {
        /* 12 小时制：bit5 = PM 标志，bit4:0 = BCD 小时(1~12) */
        IsPM = (Raw & 0x20) != 0;
        Raw = Raw & 0x1F;
    }
    else
    {
        /* 24 小时制：bit5:0 = BCD 小时(0~23) */
        Raw = Raw & 0x3F;
    }

    int Hour = DS3231_BCDToDec(Raw) % 12;   /* 12 点按 0 处理 */
    if (IsPM) Hour += 12;
    return Hour;
}

/* 时间字段范围校验：防芯片垃圾值(读)与非法入参(写)，与 OSF 状态无关 */
static bool DS3231_TimeFieldsOk(const struct tm* Time)
{
    if (Time == NULL)
    {
        return false;
    }

    if (Time->tm_sec  < 0   || Time->tm_sec  > 59  ||
        Time->tm_min  < 0   || Time->tm_min  > 59  ||
        Time->tm_hour < 0   || Time->tm_hour > 23  ||
        Time->tm_wday < 0   || Time->tm_wday > 6   ||
        Time->tm_mday < 1   || Time->tm_mday > 31  ||
        Time->tm_mon  < 0   || Time->tm_mon  > 11  ||
        Time->tm_year < 100 || Time->tm_year > 199)
    {
        return false;
    }

    return true;
}

esp_err_t DS3231_Init(void)
{
    I2C_DeviceRegister(DS3231_DEVICE_ADDR, &s_I2C_DS3231_DevHandler);

    uint8_t StatusReg = 0;
    MYESP_ERR_CHECK(DS3231_Probe(&StatusReg));

    /* 写回 0Fh：清 EN32kHz(bit3) 省电；A1F/A2F(bit0/1) 写 0 清除防残留；OSF 只读不受影响 */
    StatusReg &= ~((1 << EN32KHZ_BIT) | (1 << A1IE_BIT) | (1 << A2IE_BIT));
    MYESP_ERR_CHECK(DS3231_WriteReg(STATUS_REG_ADDR, StatusReg));

    MYESP_ERR_CHECK(DS3231_SetClockMode(false));

    uint8_t CtrlReg = 0;
    MYESP_ERR_CHECK(DS3231_ReadReg(CTRL_REG_ADDR, &CtrlReg, 1));
    // 设置EOSC为0，确保振荡器运行
    CtrlReg &= ~(1 << EOSC_BIT);
    CtrlReg &= ~(1 << A1IE_BIT);
    CtrlReg &= ~(1 << A2IE_BIT);
    MYESP_ERR_CHECK(DS3231_WriteReg(CTRL_REG_ADDR, CtrlReg));

    return  ESP_OK;
}

esp_err_t DS3231_Probe(uint8_t* StatusReg)
{
    MYESP_ERR_CHECK(DS3231_ReadReg(STATUS_REG_ADDR, StatusReg, 1));
    if ((*StatusReg) & (1 << OSF_BIT))
    {
        ESP_LOGI(DS3231_TAG, "Init: OSF is 1. Time is invalid. Please check the error.");
        s_OSF = 1;
    }
    else
    {
        s_OSF = 0;
    }
    return ESP_OK;
}

esp_err_t DS3231_GetTime(struct tm* Time)
{
    uint8_t Raw[7] = { 0 };

    /* 一次事务从 0x00 连读 7 字节（秒→年），避免多次读间的跨秒不一致 */
    MYESP_ERR_CHECK(DS3231_ReadReg(TIME_REG_ADDR, Raw, sizeof(Raw)));

    struct tm TempTime = { 0 };
    TempTime.tm_sec  = DS3231_BCDToDec(Raw[0]);
    TempTime.tm_min  = DS3231_BCDToDec(Raw[1]);
    TempTime.tm_hour = DS3231_DecodeHour(Raw[2]);
    TempTime.tm_wday = (Raw[3] & 0x07) - 1;                 /* 星期 1~7 → 0~6 */
    TempTime.tm_mday = DS3231_BCDToDec(Raw[4]);
    TempTime.tm_mon  = DS3231_BCDToDec(Raw[5] & 0x7F) - 1;  /* 掩世纪位 */
    TempTime.tm_year = DS3231_BCDToDec(Raw[6]) + 100;       /* 2000 基准 → 1900 基准 */

    if (!DS3231_TimeFieldsOk(&TempTime))
    {
        ESP_LOGI(DS3231_TAG, "GetTime: raw time out of range.");
        return ESP_FAIL;
    }

    *Time = TempTime;

    return ESP_OK;
}

bool DS3231_TimeIsValid(void)
{
    /* Init 读到的 OSF 结果：0 = 振荡器运行且时间有效 */
    return (s_OSF == 0);
}

esp_err_t DS3231_SetClockMode(uint8_t bIs12)
{
    uint8_t HourReg = 0;
    MYESP_ERR_CHECK(DS3231_ReadReg(HOUR_REG_ADDR, &HourReg, 1));

    uint8_t ModeBit = (1 << AM_PM_SWITCH_BIT);
    if (!!(HourReg & ModeBit) == !!bIs12)
    {
        return ESP_OK;    /* 已是目标模式：不写，避免写时间寄存器清 OSF */
    }

    HourReg &= ~ModeBit;  /* 只翻模式位，保留小时值 */
    if (bIs12) HourReg |= ModeBit;

    MYESP_ERR_CHECK(DS3231_WriteReg(HOUR_REG_ADDR, HourReg));
    return ESP_OK;
}

esp_err_t DS3231_SetTime(const struct tm* Time)
{
    if (Time == NULL || !DS3231_TimeFieldsOk(Time))
    {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t Raw[7];
    Raw[0] = DS3231_DecToBCD(Time->tm_sec);
    Raw[1] = DS3231_DecToBCD(Time->tm_min);
    Raw[2] = DS3231_DecToBCD(Time->tm_hour);            /* Init 已固定 24h 制，无模式位 */
    Raw[3] = (uint8_t)Time->tm_wday + 1;                /* 星期寄存器 1~7，非 BCD */
    Raw[4] = DS3231_DecToBCD(Time->tm_mday);
    Raw[5] = DS3231_DecToBCD(Time->tm_mon + 1);         /* 世纪位写 0（2000~2099） */
    Raw[6] = DS3231_DecToBCD(Time->tm_year - 100);      /* 1900 基准 → 两位数年 */

    /* 一次事务连写 00h~06h；写时间寄存器会清除芯片 OSF（校时即恢复有效） */
    MYESP_ERR_CHECK(DS3231_WriteRegs(TIME_REG_ADDR, Raw, sizeof(Raw)));

    s_OSF = 0;    /* 同步本地 OSF 缓存，恢复时间有效状态 */

    return ESP_OK;
}
