#include "pointer.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hid_device.h"
#include "settings.h"

static const char *TAG = "pointer";

/* 虚拟光标当前位置（自设备启动以来的累计位移） */
static int32_t s_x;
static int32_t s_y;

/* 校准状态 */
static bool s_calibrating;
static cal_target_t s_cal_target;
static int32_t s_cal_start_x;
static int32_t s_cal_start_y;

void pointer_init(void)
{
    s_x = 0;
    s_y = 0;
    s_calibrating = false;
    s_cal_target = CAL_TARGET_NEXT;
    s_cal_start_x = 0;
    s_cal_start_y = 0;
    ESP_LOGI(TAG, "virtual pointer initialized at (0,0)");
}

void pointer_track(int32_t dx, int32_t dy)
{
    s_x += dx;
    s_y += dy;
}

void pointer_get_position(int32_t *x, int32_t *y)
{
    if (x) {
        *x = s_x;
    }
    if (y) {
        *y = s_y;
    }
}

esp_err_t pointer_calibration_begin(cal_target_t target)
{
    s_calibrating = true;
    s_cal_target = target;
    s_cal_start_x = s_x;
    s_cal_start_y = s_y;
    ESP_LOGI(TAG, "calibration begin for target %d, start=(%d,%d)", target, s_x, s_y);
    return ESP_OK;
}

bool pointer_is_calibrating(cal_target_t target)
{
    return s_calibrating && s_cal_target == target;
}

esp_err_t pointer_calibration_end(cal_target_t target)
{
    if (!s_calibrating || s_cal_target != target) {
        return ESP_ERR_INVALID_STATE;
    }

    int32_t dx = s_x - s_cal_start_x;
    int32_t dy = s_y - s_cal_start_y;

    settings_t cfg;
    settings_load(&cfg);

    if (target == CAL_TARGET_NEXT) {
        cfg.next_dx = dx;
        cfg.next_dy = dy;
        cfg.next_calibrated = true;
    } else {
        cfg.prev_dx = dx;
        cfg.prev_dy = dy;
        cfg.prev_calibrated = true;
    }
    settings_save(&cfg);

    s_calibrating = false;
    ESP_LOGI(TAG, "calibration done for target %d: delta=(%d,%d)", target, dx, dy);
    return ESP_OK;
}

esp_err_t pointer_move_and_click(int32_t dx, int32_t dy)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "move (%d,%d) then click", dx, dy);

    /* 分步移动，模拟真人移动轨迹，避免被主机关联程序判定为瞬移 */
    const int steps = 10;
    int32_t step_x = dx / steps;
    int32_t step_y = dy / steps;

    for (int i = 0; i < steps; i++) {
        int32_t mx = step_x;
        int32_t my = step_y;
        if (i == steps - 1) {
            mx = dx - step_x * (steps - 1);
            my = dy - step_y * (steps - 1);
        }
        /* 单次发送限制在 ±127（int8 报告范围） */
        if (mx > 127)  mx = 127;
        if (mx < -127) mx = -127;
        if (my > 127)  my = 127;
        if (my < -127) my = -127;

        hid_mouse_move((int8_t)mx, (int8_t)my);
        pointer_track(mx, my);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    vTaskDelay(pdMS_TO_TICKS(60));
    ESP_RETURN_ON_ERROR(hid_mouse_click(HID_MOUSE_LEFT), TAG, "click failed");
    return ESP_OK;
}
