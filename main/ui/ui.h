/**
 * @file ui.h
 * @brief BiliMouse 屏幕 UI（自研轻量渲染器，零外部依赖）
 *
 * 设计目标：
 *   1. 2.0 寸 IPS 屏是用户躺着时唯一的状态信息来源
 *   2. 触摸目标必须够大 —— 躺着单手戳，手指精度低，最小 70x56 像素
 *   3. 屏幕本身即触控板：空白处拖动 = 移动鼠标指针
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 屏上显示的连接状态 */
typedef enum {
    UI_CONN_DISCONNECTED = 0,  /* 未连接 */
    UI_CONN_PAIRING,           /* 配对中 */
    UI_CONN_CONNECTED,        /* 已连接 */
} ui_conn_state_t;

/** 运行模式 */
typedef enum {
    UI_MODE_REMOTE = 0,   /* 遥控模式 */
    UI_MODE_MOUSE,        /* 鼠标模式 */
    UI_MODE_CALIBRATE,    /* 校准模式 */
    UI_MODE_TOUCH_CAL,    /* 触摸校准向导 */
} ui_mode_t;

/** 触摸校准界面状态 */
typedef struct {
    int  stage;            /* 0/1/2 = 采集第 1/2/3 点，3 = 验证 */
    int  target_x, target_y; /* 目标点（屏幕坐标） */
    int  point_x, point_y;   /* 当前触点（屏幕坐标） */
    bool has_point;
} ui_touch_cal_state_t;

/** 屏上可点击的按钮 */
typedef enum {
    UI_BTN_NONE = 0,
    /* 遥控模式 */
    UI_BTN_PREV_EPISODE,     /* 上一集 */
    UI_BTN_NEXT_EPISODE,     /* 下一集 */
    UI_BTN_PLAY_PAUSE,       /* 播放/暂停 */
    UI_BTN_VOLUME_UP,        /* 音量+ */
    UI_BTN_VOLUME_DOWN,      /* 音量- */
    UI_BTN_FULLSCREEN,       /* 全屏 */
    UI_BTN_MOUSE_MODE,       /* 鼠标模式开关 */
    /* 校准模式 */
    UI_BTN_CAL_NEXT,         /* 校准：下一集 */
    UI_BTN_CAL_PREV,         /* 校准：上一集 */
    UI_BTN_CAL_EXIT,         /* 校准：完成 */
    UI_BTN_MAX
} ui_button_t;

/** 用户点击按钮时回调 */
typedef void (*ui_button_cb_t)(ui_button_t btn, void *ctx);

/** 初始化 UI（创建双缓冲） */
esp_err_t ui_init(void);

/** 设置按钮点击回调 */
void ui_set_button_callback(ui_button_cb_t cb, void *ctx);

/** 设置连接状态 */
void ui_set_conn_state(ui_conn_state_t state);

/** 设置运行模式 */
void ui_set_mode(ui_mode_t mode);

/** 显示一次动作反馈（大字 + 副标题） */
void ui_show_action(const char *title, const char *subtitle);

/** 显示提示信息（副标题为主） */
void ui_show_hint(const char *hint);

/** 设置底部提示条内容 */
void ui_set_footer(const char *footer);

/**
 * 命中测试：屏幕坐标 -> 按钮
 * @return 命中的按钮，空白处返回 UI_BTN_NONE
 */
ui_button_t ui_hit_test(uint16_t x, uint16_t y);

/** 标记需要重绘 */
void ui_mark_dirty(void);

/** 更新触摸校准界面内容（仅在 UI_MODE_TOUCH_CAL 下绘制） */
void ui_touch_cal_render(const ui_touch_cal_state_t *st);

/** 刷新到屏幕（若脏标记则重绘） */
void ui_flush(void);

#ifdef __cplusplus
}
#endif
