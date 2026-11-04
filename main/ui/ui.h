/**
 * @file ui.h
 * @brief BiliMouse 屏幕 UI（自研轻量渲染器，零外部依赖）
 *
 * 设计目标：2.0 寸 IPS 屏是用户躺着时唯一的状态信息来源，
 * 因此界面必须「大字、少字、状态明确」。
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
} ui_mode_t;

/** 初始化 UI（创建双缓冲） */
esp_err_t ui_init(void);

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

/** 标记需要重绘（由 LVGL/主循环调用） */
void ui_mark_dirty(void);

/** 刷新到屏幕（若脏标记则重绘） */
void ui_flush(void);

#ifdef __cplusplus
}
#endif
