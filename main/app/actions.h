/**
 * @file actions.h
 * @brief 动作映射层：把语义动作翻译成具体的 HID 报告序列
 *
 * B 站网页端可用快捷键（已核对官方与社区实现）：
 *   空格 / K   -> 播放、暂停
 *   左 / 右方向 -> 后退 / 前进 5 秒
 *   上 / 下方向 -> 音量增 / 减
 *   F          -> 全屏
 *   M          -> 静音
 *   D          -> 弹幕开关
 *   [ / ]      -> 多 P 的上一个 / 下一个
 *
 * 注意：B 站网页端没有"下一集"的官方键盘快捷键（只有多 P 场景的 [ ]），
 *       因此"切下一集"以鼠标点击播放器上的按钮为主，键盘方案为兜底。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 语义动作 */
typedef enum {
    ACTION_NEXT_EPISODE = 0,  /* 下一集 */
    ACTION_PREV_EPISODE,      /* 上一集 */
    ACTION_PLAY_PAUSE,        /* 播放/暂停 */
    ACTION_SEEK_FORWARD,      /* 快进 5 秒 */
    ACTION_SEEK_BACKWARD,     /* 后退 5 秒 */
    ACTION_VOLUME_UP,         /* 音量+ */
    ACTION_VOLUME_DOWN,       /* 音量- */
    ACTION_FULLSCREEN,        /* 全屏 */
    ACTION_DANMAKU_TOGGLE,    /* 弹幕开关 */
    ACTION_MUTE_TOGGLE,       /* 静音 */
    ACTION_FAST_FORWARD_10S,  /* 快进 10 秒（甩腕） */
    ACTION_CLICK_LEFT,        /* 鼠标左键点击 */
    ACTION_SCROLL_UP,         /* 滚轮上滚 */
    ACTION_SCROLL_DOWN,       /* 滚轮下滚 */
    ACTION_MAX
} action_t;

/** 动作执行上下文 */
typedef struct {
    bool     next_calibrated;  /* "下一集"是否已校准 */
    int32_t  next_dx;
    int32_t  next_dy;
    bool     prev_calibrated;
    int32_t  prev_dx;
    int32_t  prev_dy;
} action_ctx_t;

/**
 * 执行一个语义动作。
 * @return ESP_OK 表示动作已发出；ESP_ERR_INVALID_STATE 表示未连接，动作被跳过。
 */
esp_err_t action_execute(action_t action, const action_ctx_t *ctx);

/** 动作对应的中文显示名（用于屏幕反馈） */
const char *action_label(action_t action);

#ifdef __cplusplus
}
#endif
