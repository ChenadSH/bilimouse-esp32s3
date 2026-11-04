#include "calibrate.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "pointer.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "calibrate";

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
    ui_show_action("校准模式", "选一个要校准的按钮");
    ui_set_footer("校准后点两次确认");
    ESP_LOGI(TAG, "enter calibration mode");
}

void calibrate_exit(void)
{
    s_active = false;
    s_state = CAL_STATE_IDLE;
    ui_set_mode(UI_MODE_REMOTE);
    ui_show_action("校准完成", "点屏幕按钮或按实体键");
    ui_set_footer("点屏幕按钮，或按实体键");
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
 * 点击某个校准按钮：
 *   第一次点击 = 记录起点
 *   第二次点击 = 确认并保存位移差
 */
void calibrate_select_target(cal_target_t target)
{
    const char *label = (target == CAL_TARGET_NEXT) ? "下一集" : "上一集";

    if (s_state == CAL_STATE_IDLE || s_target != target) {
        /* 第一次点击（或切换目标）：记录起点 */
        pointer_calibration_begin(target);
        s_target = target;
        s_state = CAL_STATE_ARMED;
        char hint[40];
        snprintf(hint, sizeof(hint), "移到%s后再点一次", label);
        ui_show_action(label, hint);
        ui_set_footer("再点一次确认");
        ESP_LOGI(TAG, "armed calibration for %s", label);
    } else {
        /* 第二次点击：确认并保存 */
        if (pointer_calibration_end(target) == ESP_OK) {
            s_state = CAL_STATE_DONE;
            char sub[32];
            snprintf(sub, sizeof(sub), "%s 已记录", label);
            ui_show_action(label, sub);
            ui_set_footer("点完成退出校准");
            ESP_LOGI(TAG, "calibration saved for %s", label);
        }
    }
}
