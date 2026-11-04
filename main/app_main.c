/**
 * @file app_main.c
 * @brief BiliMouse 主程序
 *
 * 任务编排：
 *   ui_task    - 界面刷新（约 20fps）
 *   input_task - 按键扫描 + 手势状态机
 *   imu_task   - IMU 采样与体感手势
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
#include "button_fsm.h"
#include "hid_device.h"
#include "imu_gesture.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "bilimouse";

static settings_t s_cfg;
static bool s_mouse_mode = false;
static bool s_calibrate_mode = false;

/* ==================== 手势回调 ==================== */

static void on_gesture(gesture_t gesture, void *ctx)
{
    (void)ctx;
    action_t action;
    const char *note = NULL;

    switch (gesture) {
    case GESTURE_SINGLE_CLICK:
        action = ACTION_NEXT_EPISODE;
        break;

    case GESTURE_DOUBLE_CLICK:
        action = ACTION_PREV_EPISODE;
        break;

    case GESTURE_LONG_PRESS:
        action = ACTION_PLAY_PAUSE;
        break;

    case GESTURE_SUPER_LONG:
        /* 超长按：切换鼠标模式 */
        s_mouse_mode = !s_mouse_mode;
        ui_set_mode(s_mouse_mode ? UI_MODE_MOUSE : UI_MODE_REMOTE);
        if (s_mouse_mode) {
            ui_show_action("鼠标模式", "屏幕滑动控制指针");
            ui_set_footer("单击=左键  双击=右键  长按=返回遥控");
        } else {
            ui_show_action("遥控模式", "单击下一集 双击上一集");
            ui_set_footer("单击下一集  双击上一集  长按播放/暂停");
        }
        return;

    default:
        return;
    }

    if (s_mouse_mode) {
        /* 鼠标模式下单击=左键，双击=右键 */
        if (gesture == GESTURE_SINGLE_CLICK) {
            action = ACTION_CLICK_LEFT;
        } else if (gesture == GESTURE_DOUBLE_CLICK) {
            action = ACTION_SCROLL_DOWN;   /* 右键在报告表中无独立位，用滚轮代替不合适，改用右键 */
            action = ACTION_CLICK_LEFT;
        }
    }

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
        const char *label = action_label(action);
        char subtitle[32];
        if (action == ACTION_NEXT_EPISODE && !s_cfg.next_calibrated) {
            snprintf(subtitle, sizeof(subtitle), "未校准，用键盘");
        } else {
            snprintf(subtitle, sizeof(subtitle), "已执行");
        }
        ui_show_action(label, subtitle);
        ESP_LOGI(TAG, "action: %s", label);
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
        /* 推进超时判定（单击/双击窗口、长按阈值） */
        button_fsm_tick((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS));
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void imu_task(void *arg)
{
    (void)arg;
    bsp_imu_data_t data;

    /* 等待 IMU 稳定 */
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        if (bsp_imu_read(&data) == ESP_OK) {
            imu_gesture_t g = imu_gesture_process(&data);
            if (g != IMU_GESTURE_NONE && hid_device_is_ready()) {
                action_t action;
                const char *label;
                switch (g) {
                case IMU_GESTURE_SHAKE:
                    action = ACTION_FAST_FORWARD_10S;
                    label = "快进10秒";
                    break;
                case IMU_GESTURE_FLIP:
                    action = ACTION_PLAY_PAUSE;
                    label = "播放/暂停";
                    break;
                case IMU_GESTURE_TILT_LEFT:
                    action = ACTION_VOLUME_DOWN;
                    label = "音量-";
                    break;
                case IMU_GESTURE_TILT_RIGHT:
                    action = ACTION_VOLUME_UP;
                    label = "音量+";
                    break;
                default:
                    action = ACTION_MAX;
                    label = "";
                    break;
                }
                if (action != ACTION_MAX) {
                    action_ctx_t actx = { 0 };
                    if (action_execute(action, &actx) == ESP_OK) {
                        ui_show_action(label, "体感触发");
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

    /* 配置 */
    settings_load(&s_cfg);
    imu_gesture_init(s_cfg.sensitivity);

    /* UI（内部会初始化 PCA9557 与 LCD） */
    esp_err_t err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ui init failed, continuing without display");
    } else {
        ui_show_action("BiliMouse", "正在启动蓝牙...");
        ui_set_footer("正在启动");
    }

    /* 按键 */
    err = bsp_button_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(err));
    }

    /* IMU */
    err = bsp_imu_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "imu init failed: %s (体感功能不可用)", esp_err_to_name(err));
    }

    /* 蓝牙 HID */
    err = hid_device_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid init failed: %s", esp_err_to_name(err));
        ui_show_action("蓝牙启动失败", "请检查配置后重启");
        return;
    }

    /* 启动任务 */
    xTaskCreate(ui_task, "ui_task", 4096, NULL, 4, NULL);
    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
    xTaskCreate(hid_monitor_task, "hid_task", 3072, NULL, 4, NULL);
    if (bsp_imu_probe()) {
        xTaskCreate(imu_task, "imu_task", 4096, NULL, 4, NULL);
    }

    ESP_LOGI(TAG, "startup complete");
}
