#ifndef __DS3231_H
#define __DS3231_H
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define DS3231_DEVICE_ADDR 0x68

esp_err_t DS3231_Init();

esp_err_t DS3231_GetTime(struct tm* Time);

esp_err_t DS3231_SetTime(const struct tm* Time);

esp_err_t DS3231_SetClockMode(uint8_t bIs12);

bool DS3231_TimeIsValid(void);    /* Init 读到的 OSF 结果：true = 时间有效 */

#endif
