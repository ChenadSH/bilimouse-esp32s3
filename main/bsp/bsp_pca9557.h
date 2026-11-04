/**
 * @file bsp_pca9557.h
 * @brief PCA9557 IO 扩展芯片驱动
 *
 * 板载用途：
 *   IO0 -> LCD_CS   液晶屏片选（低电平有效）
 *   IO1 -> PA_EN    音频功放使能
 *   IO2 -> DVP_PWDN 摄像头掉电控制
 *
 * 重要：上电默认 IO 为输入状态，LCD_CS 悬空会导致屏幕完全无响应。
 *      必须先配置为输出并拉低 IO0，屏幕才能正常初始化。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 I2C 总线并配置 PCA9557 */
esp_err_t bsp_pca9557_init(void);

/** 设置指定 IO 输出电平 */
esp_err_t bsp_pca9557_set_pin(uint8_t bit, bool level);

/** 读取指定 IO 输入电平 */
esp_err_t bsp_pca9557_get_pin(uint8_t bit, bool *level);

/** 释放 LCD 片选（IO0 拉低），屏幕退出复位态 */
esp_err_t bsp_pca9557_release_lcd(void);

#ifdef __cplusplus
}
#endif
