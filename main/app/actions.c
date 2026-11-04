#include "actions.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hid_device.h"
#include "hid_dev.h"
#include "pointer.h"

static const char *TAG = "actions";

/* 校准点击时移动的分步数与步间延时 */
#define CLICK_MOVE_STEPS  10

esp_err_t action_execute(action_t action, const action_ctx_t *ctx)
{
    if (!hid_device_is_ready()) {
        ESP_LOGW(TAG, "not connected, action skipped");
        return ESP_ERR_INVALID_STATE;
    }

    switch (action) {
    case ACTION_NEXT_EPISODE:
        if (ctx && ctx->next_calibrated) {
            /* 已校准：沿校准路径移动到"下一集"按钮后左键点击 */
            ESP_RETURN_ON_ERROR(pointer_move_and_click(ctx->next_dx, ctx->next_dy),
                                TAG, "move+click failed");
        } else {
            /* 未校准：回退到键盘方案（多 P 场景有效） */
            ESP_LOGW(TAG, "next episode not calibrated, fallback to ']' key");
            ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_RIGHT_BRKT, HID_MOD_NONE), TAG, "key failed");
        }
        break;

    case ACTION_PREV_EPISODE:
        if (ctx && ctx->prev_calibrated) {
            ESP_RETURN_ON_ERROR(pointer_move_and_click(ctx->prev_dx, ctx->prev_dy),
                                TAG, "move+click failed");
        } else {
            ESP_LOGW(TAG, "prev episode not calibrated, fallback to '[' key");
            ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_LEFT_BRKT, HID_MOD_NONE), TAG, "key failed");
        }
        break;

    case ACTION_PLAY_PAUSE:
        /* 空格在 B 站是播放/暂停，且页面级生效（不依赖播放器焦点） */
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_SPACEBAR, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_SEEK_FORWARD:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_RIGHT_ARROW, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_SEEK_BACKWARD:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_LEFT_ARROW, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_VOLUME_UP:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_UP_ARROW, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_VOLUME_DOWN:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_DOWN_ARROW, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_FULLSCREEN:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_F, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_DANMAKU_TOGGLE:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_D, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_MUTE_TOGGLE:
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_M, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_FAST_FORWARD_10S:
        /* 连续两次右方向键 = 前进 10 秒 */
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_RIGHT_ARROW, HID_MOD_NONE), TAG, "key failed");
        vTaskDelay(pdMS_TO_TICKS(80));
        ESP_RETURN_ON_ERROR(hid_keyboard_tap(HID_KEY_RIGHT_ARROW, HID_MOD_NONE), TAG, "key failed");
        break;

    case ACTION_CLICK_LEFT:
        /* 注意：必须用 hid_device.h 里的按键掩码(0x01/0x02)，
           不能用 hid_dev.h 的 HID_MOUSE_LEFT=253 —— 那是官方 demo
           遗留的旧 cmd 值，直接当掩码发等于同时按下 6 个键。 */
        ESP_RETURN_ON_ERROR(hid_mouse_click(HID_MOUSE_BTN_LEFT), TAG, "click failed");
        break;

    case ACTION_SCROLL_UP:
        ESP_RETURN_ON_ERROR(hid_mouse_scroll(-1), TAG, "scroll failed");
        break;

    case ACTION_SCROLL_DOWN:
        ESP_RETURN_ON_ERROR(hid_mouse_scroll(1), TAG, "scroll failed");
        break;

    default:
        return ESP_ERR_INVALID_ARG;
    }

    return ESP_OK;
}

const char *action_label(action_t action)
{
    switch (action) {
    case ACTION_NEXT_EPISODE:     return "下一集";
    case ACTION_PREV_EPISODE:     return "上一集";
    case ACTION_PLAY_PAUSE:       return "播放/暂停";
    case ACTION_SEEK_FORWARD:     return "快进";
    case ACTION_SEEK_BACKWARD:    return "后退";
    case ACTION_VOLUME_UP:        return "音量+";
    case ACTION_VOLUME_DOWN:      return "音量-";
    case ACTION_FULLSCREEN:       return "全屏";
    case ACTION_DANMAKU_TOGGLE:   return "弹幕开关";
    case ACTION_MUTE_TOGGLE:      return "静音";
    case ACTION_FAST_FORWARD_10S: return "快进10秒";
    case ACTION_CLICK_LEFT:       return "点击";
    case ACTION_SCROLL_UP:        return "上滚";
    case ACTION_SCROLL_DOWN:      return "下滚";
    default:                      return "";
    }
}
