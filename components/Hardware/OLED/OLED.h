#ifndef __OLED_H
#define __OLED_H

#include "esp_err.h"
#include <stdint.h>

/*
 * SSD1306 0.96" OLED 驱动（128x64，I2C）
 * 设计见《模块设计/驱动/OLED.md》；只被显示模块（Display）使用
 */

/* 7bit I2C 地址（与江协 8bit 写法 0x78 对应的同一个器件） */
#define OLED_DEVICE_ADDR 0x3C

/* 注册设备 + 初始化命令序列 + 清屏（app_main 前段调用一次，内部含 100ms 上电延时） */
esp_err_t OLED_Init(void);

/* 全屏清 0（水平寻址指针跨事务保持，1 次设指针 + 4 次 256B 连续写） */
esp_err_t OLED_Clear(void);

/* 清一个逻辑行（Row 0~3，每行 = 2 个页带 = 16 像素高） */
esp_err_t OLED_ClearLine(uint8_t Row);

/* 渲染一行 UTF-8 混排文本（ASCII 8px / 汉字 16px），短文本水平居中，整行单次数据事务 */
esp_err_t OLED_ShowTextLine(uint8_t Row, const char* Utf8);

#endif
