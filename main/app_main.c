/**
 * @file app_main.c
 * @brief BiliMouse 主程序
 *
 * 任务编排：
 *   ui_task      - 界面刷新（约 20fps）
 *   input_task   - 按键扫描 + 手势状态机
 *   touch_task   - 触摸：鼠标模式下拖动 = 移动指针；校准模式下确认
 *   imu_task     - IMU 采样与体感手势
 *   hid_monitor_task - 蓝牙连接状态同步
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "actions.h"
#include "bsp_button.h"
#include "bsp_imu.h"
#include "bsp_touch.h"
#include "button_fsm.h"
#include "calibrate.h"
#include "hid_device.h"
#include "imu_gesture.h"
#include "pointer.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "bilimouse";

static settings_t s_cfg;
static bool s_mouse_mode = false;

/* 前向声明 */
static void perform_action(action_t action);
static void on_ui_button(ui_button_t btn, void *ctx);

/* ==================== 模式切换 ==================== */

static void set_mouse_mode(bool on)
{
    s_mouse_mode = on;
    ui_set_mode(on ? UI_MODE_MOUSE : UI_MODE_REMOTE);
    if (on) {
        ui_show_action("鼠标模式", "屏幕拖动控制指针");
        ui_set_footer("滑动=移动  单击=左键  双击=校准");
    } else {
        ui_show_action("遥控模式", "点屏幕按钮操作");
        ui_set_footer("点屏幕按钮，或按实体键");
    }
}

/* ==================== 屏幕按钮回调 ==================== */

static void on_ui_button(ui_button_t btn, void *ctx)
{
    (void)ctx;

    switch (btn) {
    case UI_BTN_PREV_EPISODE:
        perform_action(ACTION_PREV_EPISODE);
        break;
    case UI_BTN_NEXT_EPISODE:
        perform_action(ACTION_NEXT_EPISODE);
        break;
    case UI_BTN_PLAY_PAUSE:
        perform_action(ACTION_PLAY_PAUSE);
        break;
    case UI_BTN_VOLUME_UP:
        perform_action(ACTION_VOLUME_UP);
        break;
    case UI_BTN_VOLUME_DOWN:
        perform_action(ACTION_VOLUME_DOWN);
        break;
    case UI_BTN_FULLSCREEN:
        perform_action(ACTION_FULLSCREEN);
        break;
    case UI_BTN_MOUSE_MODE:
        set_mouse_mode(!s_mouse_mode);
        break;

    /* 校准模式按钮 */
    case UI_BTN_CAL_NEXT:
        calibrate_select_target(CAL_TARGET_NEXT);
        break;
    case UI_BTN_CAL_PREV:
        calibrate_select_target(CAL_TARGET_PREV);
        break;
    case UI_BTN_CAL_EXIT:
        calibrate_exit();
        break;

    default:
        break;
    }
}

/* ==================== 动作执行 ==================== */

static void perform_action(action_t action)
{
    if (!hid_device_is_ready()) {
        ui_show_action(action_label(action), "未连接");
        ESP_LOGW(TAG, "action skipped: not connected");
        return;
    }

    action_ctx_t actx = {
        .next_calibrated = s_cfg.next_calibrated,
        .next_dx = s_cfg.next_dx,
        .next_dy = s_cfg.next_dy,
        .prev_calibrated = s_cfg.prev_calibrated,
        .prev_dx = s_cfg.prev_dx,
        .prev_dy = s_cfg.prev_dy,
    };

    if (action_execute(action, &actx) == ESP_OK) {
        char subtitle[32];
        if ((action == ACTION_NEXT_EPISODE && !s_cfg.next_calibrated) ||
            (action == ACTION_PREV_EPISODE && !s_cfg.prev_calibrated)) {
            snprintf(subtitle, sizeof(subtitle), "未校准，用键盘");
        } else {
            snprintf(subtitle, sizeof(subtitle), "已执行");
        }
        ui_show_action(action_label(action), subtitle);
        ESP_LOGI(TAG, "action: %s", action_label(action));
    }
}

/* ==================== 手势回调 ==================== */

static void on_gesture(gesture_t gesture, void *ctx)
{
    (void)ctx;

    switch (gesture) {
    case GESTURE_SINGLE_CLICK:
        if (s_mouse_mode) {
            perform_action(ACTION_CLICK_LEFT);
        } else {
            perform_action(ACTION_NEXT_EPISODE);
        }
        break;

    case GESTURE_DOUBLE_CLICK:
        if (s_mouse_mode) {
            /* 鼠标模式下双击 = 进入校准模式 */
            if (calibrate_is_active()) {
                calibrate_exit();
            } else {
                calibrate_enter();
            }
        } else {
            perform_action(ACTION_PREV_EPISODE);
        }
        break;

    case GESTURE_LONG_PRESS:
        perform_action(ACTION_PLAY_PAUSE);
        break;

    case GESTURE_SUPER_LONG:
        /* 超长按：切换鼠标模式 */
        set_mouse_mode(!s_mouse_mode);
        ESP_LOGI(TAG, "mouse mode: %s", s_mouse_mode ? "on" : "off");
        break;

    case GESTURE_HOLD_START:
        /* 按住提示：告诉用户松手会发生什么（动作尚未执行） */
        if (s_mouse_mode) {
            ui_show_hint("松开 = 进入校准模式");
        } else {
            ui_show_hint("松开 = 播放/暂停");
        }
        break;

    case GESTURE_HOLD_SUPER:
        ui_show_hint("松开 = 切换鼠标模式");
        break;

    default:
        break;
    }
}

/* ==================== 任务 ==================== */

static void input_task(void *arg)
{
    (void)arg;
    fsm_config_t fsm_cfg = {
        .click_max_ms        = 250,
        .double_click_gap_ms = 300,
        .long_press_ms       = s_cfg.long_press_ms,
        .super_long_press_ms = s_cfg.super_long_press_ms,
    };
    button_fsm_init(&fsm_cfg, on_gesture, NULL);

    bsp_button_state_t last = bsp_button_get_state();
    while (1) {
        bsp_button_state_t st = bsp_button_get_state();
        if (st != last) {
            last = st;
            button_fsm_update(st == BSP_BUTTON_PRESSED,
                               (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        }
        button_fsm_tick((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * 触摸任务
 *
 * 交互模型（重构后的核心）：
 *   - 屏幕本身就是触控板：任何模式下，**在空白处拖动 = 移动主机鼠标指针**
 *   - 遥控模式：屏幕上直接显示大按钮，点按钮 = 执行对应动作
 *   - 鼠标模式：全屏无按钮，纯触控板；实体键单击 = 左键
 *   - 校准模式：显示「校准下一集 / 校准上一集 / 完成」三个大按钮
 *
 * 区分「点击按钮」与「拖动指针」：按下时先做命中测试，
 * 命中按钮则本次触摸不产生指针移动；未命中则按触控板处理。
 */
static void touch_task(void *arg)
{
    (void)arg;
    bsp_touch_data_t t;
    bool was_pressed = false;
    bool on_button = false;      /* 本次触摸是否落在按钮上 */
    ui_button_t pressed_btn = UI_BTN_NONE;
    int16_t last_x = 0, last_y = 0;

    while (1) {
        if (bsp_touch_read(&t) == ESP_OK) {
            if (t.pressed) {
                if (!was_pressed) {
                    /* 触摸开始：判定是否落在按钮上 */
                    pressed_btn = ui_hit_test(t.x, t.y);
                    on_button = (pressed_btn != UI_BTN_NONE);
                    last_x = (int16_t)t.x;
                    last_y = (int16_t)t.y;
                } else if (!on_button) {
                    /* 拖动中（仅空白区）：移动主机指针 */
                    int16_t dx = (int16_t)(t.x - last_x);
                    int16_t dy = (int16_t)(t.y - last_y);
                    if ((dx || dy) && hid_device_is_ready()) {
                        int8_t mdx = (dx > 127) ? 127 : ((dx < -127) ? -127 : (int8_t)dx);
                        int8_t mdy = (dy > 127) ? 127 : ((dy < -127) ? -127 : (int8_t)dy);
                        hid_mouse_move(mdx, mdy);
                        pointer_track(mdx, mdy);
                    }
                    last_x = (int16_t)t.x;
                    last_y = (int16_t)t.y;
                }
                was_pressed = true;
            } else if (was_pressed) {
                /* 触摸结束 */
                was_pressed = false;

                if (on_button && pressed_btn != UI_BTN_NONE) {
                    /* 点击了按钮：执行对应动作 */
                    on_ui_button(pressed_btn, NULL);
                }
                on_button = false;
                pressed_btn = UI_BTN_NONE;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void imu_task(void *arg)
{
    (void)arg;
    bsp_imu_data_t data;

    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        if (bsp_imu_read(&data) == ESP_OK) {
            imu_gesture_t g = imu_gesture_process(&data);
            if (g != IMU_GESTURE_NONE && hid_device_is_ready()) {
                action_t action = ACTION_MAX;
                switch (g) {
                case IMU_GESTURE_SHAKE:       action = ACTION_FAST_FORWARD_10S; break;
                case IMU_GESTURE_FLIP:        action = ACTION_PLAY_PAUSE;      break;
                case IMU_GESTURE_TILT_LEFT:   action = ACTION_VOLUME_DOWN;     break;
                case IMU_GESTURE_TILT_RIGHT:  action = ACTION_VOLUME_UP;       break;
                default: break;
                }
                if (action != ACTION_MAX) {
                    action_ctx_t actx = { 0 };
                    if (action_execute(action, &actx) == ESP_OK) {
                        ui_show_action(action_label(action), "体感触发");
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));   /* 50Hz 采样 */
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        ui_flush();
        vTaskDelay(pdMS_TO_TICKS(50));   /* 20fps 刷新 */
    }
}

static void hid_monitor_task(void *arg)
{
    (void)arg;
    ui_conn_state_t last = UI_CONN_DISCONNECTED;

    while (1) {
        ui_conn_state_t st;
        switch (hid_device_state()) {
        case HID_STATE_READY:      st = UI_CONN_CONNECTED;    break;
        case HID_STATE_CONNECTING: st = UI_CONN_PAIRING;      break;
        default:                   st = UI_CONN_DISCONNECTED; break;
        }

        if (st != last) {
            last = st;
            ui_set_conn_state(st);
            switch (st) {
            case UI_CONN_CONNECTED:
                ui_show_action("已连接", "点屏幕按钮即可操作");
                ui_set_footer("点屏幕按钮，或按实体键");
                break;
            case UI_CONN_PAIRING:
                /* 区分「主机未订阅」与「还没配好」，给出可操作的提示 */
                if (hid_device_report_enabled()) {
                    ui_show_action("配对中", "等待主机确认");
                } else {
                    ui_show_action("已连接", "等待主机订阅输入");
                }
                ui_set_footer("设备名 BiliMouse");
                break;
            default:
                ui_show_action("未连接", "请先配对蓝牙");
                ui_set_footer("设备名 BiliMouse");
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ==================== 入口 ==================== */

void app_main(void)
{
    ESP_LOGI(TAG, "BiliMouse starting...");

    settings_load(&s_cfg);
    imu_gesture_init(s_cfg.sensitivity);
    pointer_init();
    calibrate_init();

    esp_err_t err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui init failed, continuing without display");
    } else {
        ui_set_button_callback(on_ui_button, NULL);
        ui_show_action("BiliMouse", "正在启动蓝牙...");
        ui_set_footer("正在启动");
    }

    err = bsp_button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
    }

    err = bsp_imu_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "imu init failed: %s (体感功能不可用)", esp_err_to_name(err));
    }

    bool touch_ok = (bsp_touch_init() == ESP_OK);
    if (!touch_ok) {
        ESP_LOGW(TAG, "touch init failed (触摸功能不可用)");
    }

    err = hid_device_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid init failed: %s", esp_err_to_name(err));
        ui_show_action("蓝牙启动失败", "请检查配置后重启");
        return;
    }

    xTaskCreate(ui_task, "ui_task", 4096, NULL, 4, NULL);
    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
    if (touch_ok) {
        xTaskCreate(touch_task, "touch_task", 4096, NULL, 4, NULL);
    }
    xTaskCreate(hid_monitor_task, "hid_task", 3072, NULL, 4, NULL);
    if (bsp_imu_probe()) {
        xTaskCreate(imu_task, "imu_task", 4096, NULL, 4, NULL);
    }

    ESP_LOGI(TAG, "startup complete");
}
