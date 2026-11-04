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

/** 是否可以发送 HID 报告（已连接 + 已加密 + 主机已订阅） */
bool hid_device_is_ready(void);

/** 主机是否已订阅输入报告（用于屏幕上的诊断提示） */
bool hid_device_report_enabled(void);

/** 最近一次配对是否失败（供界面提示，配对成功后自动清除） */
bool hid_device_pairing_failed(void);

/** 清除本机保存的所有配对密钥（bonds），下次连接将重新配对 */
void hid_device_clear_bonds(void);

/** 状态巡检：广播看门狗 + 配对超时保护，需定期调用（建议 100~200ms 一次） */
void hid_device_service(void);

/* ---------------- 鼠标 ---------------- */

/** 发送鼠标移动（相对位移，单步） */
esp_err_t hid_mouse_move(int8_t dx, int8_t dy);

/** 发送鼠标滚轮（正=向上滚，负=向下滚） */
esp_err_t hid_mouse_scroll(int8_t wheel);

/** 发送鼠标按键点击（按下 + 释放） */
esp_err_t hid_mouse_click(uint8_t button);

/**
 * 按住 / 松开鼠标按键（不自动释放）。
 *
 * 实体按键走这条路径，等价于一只真正的鼠标左键：
 * 单击、双击、按住拖动全部由主机系统判定。
 */
esp_err_t hid_mouse_button(uint8_t button, bool down);

/** 释放所有鼠标按键（断连或切模式时兜底，同时释放键盘修饰键） */
void hid_mouse_release_all(void);

/** 按住 / 松开键盘修饰键（不自动释放，不触发按键） */
esp_err_t hid_keyboard_modifier(uint8_t modifier, bool down);

/** 鼠标按键位定义（与 HID 报告 byte0 一致） */
#define HID_MOUSE_BTN_LEFT    0x01
#define HID_MOUSE_BTN_RIGHT   0x02
#define HID_MOUSE_BTN_MIDDLE  0x04

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
 * 分步移动鼠标指针。
 *
 * 说明：BLE HID 鼠标只能发送相对位移，无法直接指定绝对坐标。
 * 真正的「定位到某个按钮」由 app/pointer.c 的虚拟光标方案实现，
 * 本函数仅提供轨迹平滑的分步移动原语。
 *
 * @param total_dx  总 X 位移
 * @param total_dy  总 Y 位移
 * @param steps     拆分步数（<=0 使用默认值）
 */
esp_err_t hid_mouse_move_relative_steps(int32_t total_dx, int32_t total_dy, int steps);

#ifdef __cplusplus
}
#endif
