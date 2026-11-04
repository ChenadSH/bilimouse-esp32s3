/**
 * @file bsp_button.h
 * @brief 用户按键驱动（GPIO0，低电平按下，带消抖）
 */
#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BSP_BUTTON_PRESSED = 0,   /* 低电平按下 */
    BSP_BUTTON_RELEASED = 1,
} bsp_button_state_t;

/** 初始化按键 GPIO（内部上拉 + 下降沿中断） */
esp_err_t bsp_button_init(void);

/** 读取按键当前状态（已消抖） */
bsp_button_state_t bsp_button_get_state(void);

/** 阻塞等待按键状态变化，返回变化后的状态 */
bsp_button_state_t bsp_button_wait_change(bsp_button_state_t last, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
