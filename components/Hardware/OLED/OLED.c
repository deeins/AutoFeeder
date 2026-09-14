#include "OLED.h"
#include "I2C.h"
#include "OLED_Font.h"
#include "OLED_CN_Font.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * SSD1306 128x64 驱动（硬件 I2C，与 DS3231 共总线）
 *  - 控制字节（Co + D/C#）：0x00 = 后续全为命令；0x40 = 后续全为数据；批量事务每批只带一个
 *  - 显存模型：格 = 1 列 x 8 行，每字节管一列的 8 个纵向像素；逻辑行 = 2 个整页带，字形落页边界
 *  - 写策略：逐页写（每页 128B + 页内居中留黑），不依赖寻址模式——
 *    部分兼容芯片（如 SH1106）不支持 SSD1306 的水平寻址连续跨页写：一整行 256B 会被按页内
 *    回绕处理，导致上下半页互相覆盖；逐页写对两种芯片行为一致（2026-09-14 真机现象修正）
 */

#define OLED_SEND_CMD   0x00
#define OLED_SEND_DATA  0x40

#define OLED_WIDTH      128
#define OLED_PAGE_COUNT 8
#define OLED_LINE_COUNT 4
#define OLED_PAGE_MAX   (1 + OLED_WIDTH)         /* 控制字节 + 单页 128B 数据 */

/* 列起始偏移：SSD1306 = 0；若实物为 SH1106（132 列 RAM、可视窗自列 2 起、画面左移 2px）改为 2 */
#define OLED_COL_OFFSET 0

#define OLED_FONT_FIRST ' '
#define OLED_FONT_LAST  '~'

static i2c_master_dev_handle_t s_I2C_OLED_DevHandler = NULL;
static const char* OLED_TAG = "OLED";

/* 连续失败日志限流：前 3 次逐条，之后每 20 次一条（配合显示模块退避，防刷屏） */
static uint32_t s_FailCnt = 0;

static esp_err_t OLED_WriteRaw(const uint8_t* Data, size_t Len)
{
    esp_err_t Err = I2C_DeviceWriteRaw(s_I2C_OLED_DevHandler, Data, Len);
    if (Err != ESP_OK)
    {
        s_FailCnt++;
        if (s_FailCnt <= 3 || s_FailCnt % 20 == 0)
        {
            ESP_LOGI(OLED_TAG, "ERROR: %s. Fail to write %u bytes (consecutive %u).",
                     esp_err_to_name(Err), (unsigned)Len, (unsigned)s_FailCnt);
        }
        return Err;
    }
    s_FailCnt = 0;
    return ESP_OK;
}

/* 设显存指针到 (Page, Column)。逐页写：每次数据事务只写 128B，页指针不依赖自动翻页 */
static esp_err_t OLED_SetCursor(uint8_t Page, uint8_t Column)
{
    uint8_t Cmd[] = {
        OLED_SEND_CMD,
        0xB0 | Page,
        0x10 | ((Column & 0xF0) >> 4),
        0x00 | (Column & 0x0F),
    };

    return OLED_WriteRaw(Cmd, sizeof(Cmd));
}

/* 全零单页数据帧（静态常量表，清屏/清行复用） */
static const uint8_t s_ZeroPage[OLED_PAGE_MAX] = { OLED_SEND_DATA };

esp_err_t OLED_Init(void)
{
    /* 上电稳定；vTaskDelay 收 tick，本项目 100Hz → pdMS_TO_TICKS(100) = 100ms */
    vTaskDelay(pdMS_TO_TICKS(100));

    I2C_DeviceRegister(OLED_DEVICE_ADDR, &s_I2C_OLED_DevHandler);

    static const uint8_t InitCmds[] = {
        OLED_SEND_CMD,
        0xAE,           /* 关闭显示 */
        0xD5, 0x80,     /* 显示时钟分频比/振荡器频率 */
        0xA8, 0x3F,     /* 多路复用率 = 64 行 */
        0xD3, 0x00,     /* 显示偏移 = 0 */
        0x40,           /* 显示起始行 = 0 */
        0xA1,           /* 左右方向：正常 */
        0xC8,           /* 上下方向：正常（原点在左上） */
        0xDA, 0x12,     /* COM 引脚硬件配置 */
        0x81, 0xCF,     /* 对比度 */
        0xD9, 0xF1,     /* 预充电周期 */
        0xDB, 0x30,     /* VCOMH 取消选择级别 */
        0xA4,           /* 整体显示跟随显存 */
        0xA6,           /* 正常显示（非反色） */
        0x8D, 0x14,     /* 充电泵开 */
        0xAF,           /* 开启显示 */
    };

    MYESP_ERR_CHECK(OLED_WriteRaw(InitCmds, sizeof(InitCmds)));
    MYESP_ERR_CHECK(OLED_Clear());

    ESP_LOGI(OLED_TAG, "Init done: 128x64, addr 0x%02X.", OLED_DEVICE_ADDR);
    return ESP_OK;
}

esp_err_t OLED_Clear(void)
{
    /* 逐页清 8 页；每事务 129B ≈ 3ms@400kHz */
    for (uint8_t Page = 0; Page < OLED_PAGE_COUNT; Page++)
    {
        MYESP_ERR_CHECK(OLED_SetCursor(Page, OLED_COL_OFFSET));
        MYESP_ERR_CHECK(OLED_WriteRaw(s_ZeroPage, sizeof(s_ZeroPage)));
    }

    ESP_LOGD(OLED_TAG, "Clear: 1024 bytes in 8 pages.");
    return ESP_OK;
}

esp_err_t OLED_ClearLine(uint8_t Row)
{
    if (Row >= OLED_LINE_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (uint8_t Page = Row * 2; Page < Row * 2 + 2; Page++)
    {
        MYESP_ERR_CHECK(OLED_SetCursor(Page, OLED_COL_OFFSET));
        MYESP_ERR_CHECK(OLED_WriteRaw(s_ZeroPage, sizeof(s_ZeroPage)));
    }

    return ESP_OK;
}

/* 解码一个 UTF-8 字形；未收录 / 异常序列回退 '?' 占位（宽 8） */
typedef struct {
    const uint8_t* Bits;     /* 上半页 Width 字节 + 下半页 Width 字节 */
    uint8_t        Width;    /* 8（ASCII）/ 16（汉字） */
    uint8_t        Adv;      /* UTF-8 序列字节数（推进指针用） */
} OledGlyph_t;

static OledGlyph_t OLED_DecodeGlyph(const char* Str)
{
    OledGlyph_t Glyph = { &OLED_F8x16['?' - OLED_FONT_FIRST][0], 8, 1 };
    uint8_t Byte = (uint8_t)Str[0];

    if (Byte < 0x80)
    {
        /* 单字节 ASCII：0x20~0x7E 查表，其余（含控制符）用占位符 */
        if (Byte >= OLED_FONT_FIRST && Byte <= OLED_FONT_LAST)
        {
            Glyph.Bits = &OLED_F8x16[Byte - OLED_FONT_FIRST][0];
        }
        return Glyph;
    }

    if (Byte >= 0xF0)
    {
        Glyph.Adv = 4;      /* 4 字节序列（emoji 等）→ 占位 */
        return Glyph;
    }

    if (Byte >= 0xE0)
    {
        /* 3 字节序列：汉字 / 中文标点。注意 char 默认有符号，必须先转 uint8_t，否则 0xE5 变负 */
        Glyph.Adv = 3;
        if (Str[1] == '\0' || Str[2] == '\0')
        {
            return Glyph;
        }
        uint32_t Cp = ((uint32_t)(Byte & 0x0F) << 12) |
                      (((uint32_t)(uint8_t)Str[1] & 0x3F) << 6) |
                       ((uint32_t)(uint8_t)Str[2] & 0x3F);
        for (uint32_t i = 0; i < OLED_CN_FONT_COUNT; i++)
        {
            if (OLED_CN_Font[i].Codepoint == Cp)
            {
                Glyph.Bits = OLED_CN_Font[i].Bits;
                Glyph.Width = 16;
                return Glyph;
            }
        }
        return Glyph;       /* 未收录字 → 占位 */
    }

    /* 2 字节序列 / 游离续字节 → 占位 */
    Glyph.Adv = (Byte >= 0xC0) ? 2 : 1;
    return Glyph;
}

esp_err_t OLED_ShowTextLine(uint8_t Row, const char* Utf8)
{
    if (Row >= OLED_LINE_COUNT || Utf8 == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    /* 先扫一遍总宽（未收录按占位宽 8 计），求居中起点 */
    uint16_t TotalW = 0;
    for (const char* p = Utf8; *p != '\0';)
    {
        OledGlyph_t Glyph = OLED_DecodeGlyph(p);
        TotalW += Glyph.Width;
        p += Glyph.Adv;
    }
    uint8_t StartCol = (TotalW < OLED_WIDTH) ? (uint8_t)((OLED_WIDTH - TotalW) / 2) : 0;

    /* 上下半页各一帧（128B + 控制字节）；两侧补 0 兼作擦除旧内容 */
    uint8_t Upper[OLED_PAGE_MAX];
    uint8_t Lower[OLED_PAGE_MAX];
    memset(Upper, 0, sizeof(Upper));
    memset(Lower, 0, sizeof(Lower));
    Upper[0] = OLED_SEND_DATA;
    Lower[0] = OLED_SEND_DATA;

    uint8_t Col = StartCol;
    for (const char* p = Utf8; *p != '\0';)
    {
        OledGlyph_t Glyph = OLED_DecodeGlyph(p);
        if (Col + Glyph.Width > OLED_WIDTH)
        {
            break;          /* 放不下的尾巴截断 */
        }
        memcpy(&Upper[1 + Col], Glyph.Bits, Glyph.Width);                    /* 上半页 */
        memcpy(&Lower[1 + Col], Glyph.Bits + Glyph.Width, Glyph.Width);      /* 下半页 */
        Col += Glyph.Width;
        p += Glyph.Adv;
    }

    MYESP_ERR_CHECK(OLED_SetCursor(Row * 2, OLED_COL_OFFSET));
    MYESP_ERR_CHECK(OLED_WriteRaw(Upper, sizeof(Upper)));
    MYESP_ERR_CHECK(OLED_SetCursor(Row * 2 + 1, OLED_COL_OFFSET));
    MYESP_ERR_CHECK(OLED_WriteRaw(Lower, sizeof(Lower)));

    return ESP_OK;
}
