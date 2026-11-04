/**
 * @file bsp_lcd.h
 * @brief ST7789 LCD 驱动（ESP-IDF esp_lcd 组件）
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
    int width;
    int height;
} bsp_lcd_t;

/** 初始化 LCD：PCA9557 -> 背光 -> SPI -> 面板 */
esp_err_t bsp_lcd_init(bsp_lcd_t *lcd);

/** 在指定区域绘制 RGB565 缓冲区 */
esp_err_t bsp_lcd_draw_bitmap(bsp_lcd_t *lcd, int x1, int y1, int x2, int y2, const uint16_t *data);

/** 设置背光亮度 0~100 */
esp_err_t bsp_lcd_set_backlight(uint8_t percent);

#ifdef __cplusplus
}
#endif
