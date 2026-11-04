#include "calibrate.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "pointer.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "calibrate";

/* 校准界面上的按钮区域（屏幕 320x240） */
#define BTN_NEXT_X0   20
#define BTN_NEXT_X1   150
#define BTN_NEXT_Y0   120
#define BTN_NEXT_Y1   180

#define BTN_PREV_X0   170
#define BTN_PREV_X1   300
#define BTN_PREV_Y0   120
#define BTN_PREV_Y1   180

#define BTN_EXIT_X0   120
#define BTN_EXIT_X1   200
#define BTN_EXIT_Y0   195
#define BTN_EXIT_Y1   230

static bool s_active;
static cal_state_t s_state = CAL_STATE_IDLE;
static cal_target_t s_target = CAL_TARGET_NEXT;

void calibrate_init(void)
{
    s_active = false;
    s_state = CAL_STATE_IDLE;
    s_target = CAL_TARGET_NEXT;
}

void calibrate_enter(void)
{
    s_active = true;
    s_state = CAL_STATE_IDLE;
    ui_set_mode(UI_MODE_CALIBRATE);
    ui_show_action("校准模式", "点击要校准的按钮");
    ui_set_footer("下一集  上一集  完成");
    ESP_LOGI(TAG, "enter calibration mode");
}

void calibrate_exit(void)
{
    s_active = false;
    s_state = CAL_STATE_IDLE;
    ui_set_mode(UI_MODE_REMOTE);
    ui_show_action("校准完成", "单击即可切换视频");
    ui_set_footer("单击下一集  双击上一集  长按播放/暂停");
    ESP_LOGI(TAG, "exit calibration mode");
}

bool calibrate_is_active(void)
{
    return s_active;
}

cal_state_t calibrate_state(void)
{
    return s_state;
}

int calibrate_current_target(void)
{
    return (int)s_target;
}

/**
 * 第一次点击某个按钮 = 记录起点；第二次点击 = 确认并保存位移差
 */
static void handle_target_button(cal_target_t target, const char *label)
{
    if (s_state == CAL_STATE_IDLE) {
        /* 第一次点击：记录起点 */
        pointer_calibration_begin(target);
        s_target = target;
        s_state = CAL_STATE_ARMED;
        char hint[48];
        snprintf(hint, sizeof(hint), "用鼠标移到%s后点此确认", label);
        ui_show_action(label, hint);
        ui_set_footer("再点一次确认  或点完成退出");
        ESP_LOGI(TAG, "armed calibration for %s", label);
    } else if (s_state == CAL_STATE_ARMED && s_target == target) {
        /* 第二次点击：确认并保存 */
        if (pointer_calibration_end(target) == ESP_OK) {
            s_state = CAL_STATE_DONE;
            char sub[48];
            snprintf(sub, sizeof(sub), "%s 已记录", label);
            ui_show_action(label, sub);
            ui_set_footer("点击完成退出校准");
            ESP_LOGI(TAG, "calibration saved for %s", label);
        }
    } else {
        /* 切换到另一个目标：重新记录起点 */
        pointer_calibration_begin(target);
        s_target = target;
        s_state = CAL_STATE_ARMED;
        char hint[48];
        snprintf(hint, sizeof(hint), "用鼠标移到%s后点此确认", label);
        ui_show_action(label, hint);
    }
}

void calibrate_on_touch(uint16_t x, uint16_t y)
{
    if (!s_active) {
        return;
    }

    if (x >= BTN_NEXT_X0 && x <= BTN_NEXT_X1 && y >= BTN_NEXT_Y0 && y <= BTN_NEXT_Y1) {
        handle_target_button(CAL_TARGET_NEXT, "下一集");
        return;
    }

    if (x >= BTN_PREV_X0 && x <= BTN_PREV_X1 && y >= BTN_PREV_Y0 && y <= BTN_PREV_Y1) {
        handle_target_button(CAL_TARGET_PREV, "上一集");
        return;
    }

    if (x >= BTN_EXIT_X0 && x <= BTN_EXIT_X1 && y >= BTN_EXIT_Y0 && y <= BTN_EXIT_Y1) {
        calibrate_exit();
        return;
    }
}
