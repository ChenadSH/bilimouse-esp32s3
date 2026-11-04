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
#include "hid_device.h"
#include "imu_gesture.h"
#include "pointer.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "bilimouse";

static settings_t s_cfg;
static bool s_mouse_mode = false;

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
            perform_action(ACTION_SCROLL_DOWN);
        } else {
            perform_action(ACTION_PREV_EPISODE);
        }
        break;

    case GESTURE_LONG_PRESS:
        perform_action(ACTION_PLAY_PAUSE);
        break;

    case GESTURE_SUPER_LONG:
        /* 超长按：切换鼠标模式 */
        s_mouse_mode = !s_mouse_mode;
        ui_set_mode(s_mouse_mode ? UI_MODE_MOUSE : UI_MODE_REMOTE);
        if (s_mouse_mode) {
            ui_show_action("鼠标模式", "屏幕滑动控制指针");
            ui_set_footer("滑动=移动  单击=左键  双击=滚轮");
        } else {
            ui_show_action("遥控模式", "单击下一集 双击上一集");
            ui_set_footer("单击下一集  双击上一集  长按播放/暂停");
        }
        ESP_LOGI(TAG, "mouse mode: %s", s_mouse_mode ? "on" : "off");
        break;

    case GESTURE_HOLD_START:
        /* 按住提示：告诉用户松手会发生什么（动作尚未执行） */
        ui_show_hint("松开 = 播放/暂停");
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
 * 触摸任务：
 *   鼠标模式 - 拖动 = 相对移动鼠标指针（触控板体验）
 *   遥控模式 - 点击 = 快捷动作（下一集/播放暂停）
 */
static void touch_task(void *arg)
{
    (void)arg;
    bsp_touch_data_t t;
    bool was_pressed = false;
    int16_t last_x = 0, last_y = 0;

    while (1) {
        if (bsp_touch_read(&t) == ESP_OK) {
            if (t.pressed) {
                if (!was_pressed) {
                    /* 触摸开始 */
                    last_x = (int16_t)t.x;
                    last_y = (int16_t)t.y;
                } else {
                    /* 拖动中 */
                    int16_t dx = (int16_t)(t.x - last_x);
                    int16_t dy = (int16_t)(t.y - last_y);
                    if (dx || dy) {
                        if (s_mouse_mode && hid_device_is_ready()) {
                            /* 触控板：滑动 -> 相对移动指针 */
                            int8_t mdx = (dx > 127) ? 127 : ((dx < -127) ? -127 : (int8_t)dx);
                            int8_t mdy = (dy > 127) ? 127 : ((dy < -127) ? -127 : (int8_t)dy);
                            hid_mouse_move(mdx, mdy);
                            pointer_track(mdx, mdy);
                        }
                        last_x = (int16_t)t.x;
                        last_y = (int16_t)t.y;
                    }
                }
                was_pressed = true;
            } else if (was_pressed) {
                /* 触摸结束 */
                was_pressed = false;
                if (!s_mouse_mode && hid_device_is_ready()) {
                    /* 遥控模式：屏幕下半区点击 = 切换下一集 */
                    if (t.y > 120) {
                        perform_action(ACTION_NEXT_EPISODE);
                    } else {
                        perform_action(ACTION_PLAY_PAUSE);
                    }
                }
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
                ui_show_action("已连接", "单击即可切换视频");
                ui_set_footer("单击下一集  双击上一集  长按播放/暂停");
                break;
            case UI_CONN_PAIRING:
                ui_show_action("配对中", "请在电脑蓝牙设置中搜索");
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

    esp_err_t err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui init failed, continuing without display");
    } else {
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

    err = hid_device_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid init failed: %s", esp_err_to_name(err));
        ui_show_action("蓝牙启动失败", "请检查配置后重启");
        return;
    }

    xTaskCreate(ui_task, "ui_task", 4096, NULL, 4, NULL);
    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
    xTaskCreate(touch_task, "touch_task", 4096, NULL, 4, NULL);
    xTaskCreate(hid_monitor_task, "hid_task", 3072, NULL, 4, NULL);
    if (bsp_imu_probe()) {
        xTaskCreate(imu_task, "imu_task", 4096, NULL, 4, NULL);
    }

    ESP_LOGI(TAG, "startup complete");
}
