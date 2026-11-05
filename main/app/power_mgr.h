/**
 * @file power_mgr.h
 * @brief 省电模式：空闲变暗 -> 息屏 -> 触摸唤醒
 *
 * 三档状态：
 *   ACTIVE  正常亮度，触摸 20ms 轮询，界面 20fps 刷新
 *   DIM     背光降到 25%，界面仍刷新（还想继续看时不用摸黑找按钮）
 *   SLEEP   背光 0，界面停止刷新，触摸降到 150ms 轮询（仍然能唤醒）
 *
 * 为什么不直接进 light sleep：
 *   BLE HID 连接在 light sleep 下会断，唤醒后要重新加密配对，
 *   对"躺着按一下空格"这种低频率操作反而更烦。屏幕背光和轮询频率
 *   才是这块板子的耗电大头，先把这两项压下去，收益已经足够。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    POWER_ACTIVE = 0,
    POWER_DIM,
    POWER_SLEEP,
} power_state_t;

/** 初始化（读取设置里的亮度并立即生效） */
esp_err_t power_mgr_init(void);

/** 启动巡检任务 */
void power_mgr_start(void);

/** 有操作发生：刷新空闲计时并立即唤醒屏幕 */
void power_mgr_notify_activity(void);

/** 屏幕是否点亮（息屏时界面不必重绘） */
bool power_mgr_display_on(void);

/** 当前状态 */
power_state_t power_mgr_state(void);

/** 调整正常亮度（0~100），会同时写入设置 */
void power_mgr_set_brightness(uint8_t percent);

#ifdef __cplusplus
}
#endif
