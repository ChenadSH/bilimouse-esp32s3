/**
 * @file bsp_touch.h
 * @brief FT6336 电容触摸控制器驱动（I2C）
 *
 * 只负责"把手指按在哪里"读出来，**不做任何方向/缩放换算**：
 * 面板是竖装还是横装、坐标是 12bit 还是像素，全部交给
 * app/touch_cal.c 的三点校准矩阵处理（那才是唯一可靠的做法）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     pressed;
    uint16_t raw_x;   /* 面板原始 X（未换算） */
    uint16_t raw_y;   /* 面板原始 Y（未换算） */
} bsp_touch_data_t;

/** 初始化触摸控制器 */
esp_err_t bsp_touch_init(void);

/** 读取触摸状态（无触摸时 pressed=false，此时 raw 值无意义） */
esp_err_t bsp_touch_read(bsp_touch_data_t *data);

/** 调试：把 0x00~0x0F 寄存器整块打印出来（排查坐标始终为 0 的问题） */
esp_err_t bsp_touch_dump(void);

#ifdef __cplusplus
}
#endif
