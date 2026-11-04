/**
 * @file bsp_touch.h
 * @brief FT6336 电容触摸控制器驱动（I2C）
 *
 * 用途：校准 / 设置界面的触摸交互。
 * 触摸坐标为原始 12bit 值，需按面板实际尺寸换算并做方向映射。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     pressed;
    uint16_t x;      /* 已映射到屏幕坐标 (0..319) */
    uint16_t y;      /* 已映射到屏幕坐标 (0..239) */
} bsp_touch_data_t;

/** 初始化触摸控制器 */
esp_err_t bsp_touch_init(void);

/** 读取触摸状态（无触摸时 pressed=false） */
esp_err_t bsp_touch_read(bsp_touch_data_t *data);

#ifdef __cplusplus
}
#endif
