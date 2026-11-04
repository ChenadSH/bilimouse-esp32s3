/**
 * @file hid_device.h
 * @brief BiliMouse BLE HID 应用层
 *
 * 在官方 esp_hidd profile 之上封装：
 *   - 设备连接状态管理
 *   - 鼠标 / 键盘 / 消费类 三合一报告发送
 *   - 相对移动分步插值（模拟真人移动，规避主机关联程序的瞬移检测）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

/** HID 连接状态 */
typedef enum {
    HID_STATE_IDLE = 0,       /* 未连接 */
    HID_STATE_ADVERTISING,    /* 广播中（等待配对） */
    HID_STATE_CONNECTING,     /* 已连接，配对/加密中 */
    HID_STATE_READY,          /* 已配对，可发送报告 */
} hid_conn_state_t;

/** 初始化蓝牙控制器 + HID profile，开始广播 */
esp_err_t hid_device_init(void);

/** 当前连接状态 */
hid_conn_state_t hid_device_state(void);

/** 是否可以发送 HID 报告（已连接且已加密） */
bool hid_device_is_ready(void);

/* ---------------- 鼠标 ---------------- */

/** 发送鼠标移动（相对位移，单步） */
esp_err_t hid_mouse_move(int8_t dx, int8_t dy);

/** 发送鼠标滚轮（正=向上滚，负=向下滚） */
esp_err_t hid_mouse_scroll(int8_t wheel);

/** 发送鼠标按键点击（按下 + 释放） */
esp_err_t hid_mouse_click(uint8_t button);

/* ---------------- 键盘 ---------------- */

/** HID 修饰键 */
typedef enum {
    HID_MOD_NONE  = 0x00,
    HID_MOD_CTRL  = 0x01,
    HID_MOD_SHIFT = 0x02,
    HID_MOD_ALT   = 0x04,
    HID_MOD_GUI   = 0x08,
} hid_modifier_t;

/** 按下并释放一个普通键 */
esp_err_t hid_keyboard_tap(uint8_t keycode, uint8_t modifier);

/** 按下并释放一个组合键（modifier + key） */
esp_err_t hid_keyboard_combo(uint8_t keycode, uint8_t modifier);

/* ---------------- 消费类（媒体键） ---------------- */

/** 发送媒体键（按下+释放），cmd 取 HID_CONSUMER_* */
esp_err_t hid_consumer_tap(uint8_t cmd);

/* ---------------- 高级：分步移动到绝对坐标 ---------------- */

/**
 * 分步移动鼠标指针到绝对坐标。
 * 相对移动无法直接指定绝对位置，这里通过"读取当前光标位置"不可行，
 * 因此该函数用于配合鼠标自身的位移反馈做近似定位：
 * 调用方传入目标相对偏移量，函数会拆成多步发送，模拟真人移动轨迹。
 *
 * @param total_dx  总 X 位移
 * @param total_dy  总 Y 位移
 * @param steps     拆分步数（<=0 使用默认值）
 */
esp_err_t hid_mouse_move_relative_steps(int32_t total_dx, int32_t total_dy, int steps);

#ifdef __cplusplus
}
#endif
