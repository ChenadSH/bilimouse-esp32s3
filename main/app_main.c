/**
 * @file app_main.c
 * @brief BiliMouse 主程序
 *
 * 任务编排：
 *   ui_task      - 界面刷新（约 20fps）
 *   input_task   - 实体按键 -> 鼠标左键（按下/松开直通主机）
 *   touch_task   - 触摸：拖动 = 移动指针；点按钮 = 执行动作
 *   hid_monitor_task - 蓝牙连接状态同步
 *
 * 交互模型（第三版，按真机反馈收敛）：
 *   1. 实体按键就是鼠标左键：按下发送"左键按下"，松开发送"左键松开"。
 *      单击 / 双击 / 按住拖动全部由 macOS 自己判定，固件不猜。
 *   2. 屏幕是触控板：任意模式空白处拖动 = 移动指针。
 *   3. 每个功能都有屏上按钮，不依赖任何隐藏手势。
 *   4. 体感不再参与播放控制（床上翻身误触发太多）。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "actions.h"
#include "bsp_button.h"
#include "bsp_imu.h"
#include "bsp_touch.h"
#include "calibrate.h"
#include "hid_device.h"
#include "pointer.h"
#include "settings.h"
#include "touch_cal.h"
#include "ui.h"

static const char *TAG = "bilimouse";

/* 触摸排查开关：打开后每次按下会打印一次原始/映射坐标 */
#define TOUCH_DEBUG 1

static settings_t s_cfg;
static bool s_mouse_mode = false;
static int  s_cal_shown_stage = -1;   /* 触摸校准界面当前展示的阶段 */

/* 前向声明 */
static void perform_action(action_t action);
static void on_ui_button(ui_button_t btn, void *ctx);
static void start_touch_cal(void);

/* ==================== 模式切换 ==================== */

static void set_mouse_mode(bool on)
{
    s_mouse_mode = on;
    hid_mouse_release_all();   /* 切模式时别把主机卡在"按住"状态 */
    ui_set_mode(on ? UI_MODE_MOUSE : UI_MODE_REMOTE);
    if (on) {
        ui_show_action("鼠标模式", "全屏当触控板");
        ui_set_footer("拖动=移动  实体键=左键");
    } else {
        ui_show_action("遥控模式", "点屏幕按钮操作");
        ui_set_footer("拖动=移动  实体键=左键");
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
    case UI_BTN_TOUCH_CAL:
        start_touch_cal();
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

/* ==================== 触摸校准 ==================== */

/** 校准结束（成功或超时）后恢复原来的界面 */
static void on_touch_cal_done(bool ok)
{
    ui_set_mode(s_mouse_mode ? UI_MODE_MOUSE : UI_MODE_REMOTE);
    s_cal_shown_stage = -1;

    if (ok) {
        ui_show_action("触摸已校准", "点按钮试试");
        ui_set_footer("拖动=移动  实体键=左键");
        ESP_LOGI(TAG, "touch calibration finished");
    } else {
        ui_show_action("校准未完成", "点校准按钮可重做");
        ui_set_footer("点屏幕「校准」按钮可重做");
        ESP_LOGW(TAG, "touch calibration aborted");
    }
}

/** 进入触摸校准界面 */
static void start_touch_cal(void)
{
    if (touch_cal_is_active()) {
        return;
    }
    ui_set_mode(UI_MODE_TOUCH_CAL);
    ui_show_action("触摸校准", "按住红框，松手记录");
    ui_set_footer("3 个点，跟着红框走");
    s_cal_shown_stage = 0;
    touch_cal_start_ex(false);
}

/** 阶段变化时更新卡片文字 */
static void touch_cal_sync_ui(void)
{
    touch_cal_stage_t st = touch_cal_stage();
    if ((int)st == s_cal_shown_stage) {
        return;
    }
    s_cal_shown_stage = (int)st;

    switch (st) {
    case TOUCH_CAL_TAP1:
        ui_show_action("第 1 个点", "点住左上角的红框");
        break;
    case TOUCH_CAL_TAP2:
        ui_show_action("第 2 个点", "点住右上角的红框");
        break;
    case TOUCH_CAL_TAP3:
        ui_show_action("第 3 个点", "点住左下角的红框");
        break;
    case TOUCH_CAL_VERIFY:
        ui_show_action("校准完成", "滑动手指看十字跟不跟手");
        ui_set_footer("6 秒后自动返回");
        break;
    default:
        break;
    }
}

/* ==================== 任务 ==================== */

/**
 * 实体按键任务：把一个普通轻触按键变成真正的鼠标左键。
 *
 * 为什么不做手势识别？
 *   以前是「单击=下一集 / 双击=上一集 / 长按=播放暂停」，需要固件自己计时判断，
 *   结果三个问题一起出现：双击窗口和系统不一致、长按和拖动打架、
 *   而用户真正想要的只是「当鼠标用」。
 *
 * 现在的做法：按下就发 left-button-down，松开发 left-button-up。
 *   单击、双击、三击、按住拖动、框选，全部由 macOS 自己按系统设置判定，
 *   行为和一只有线鼠标完全一致，不存在"设备以为双击了、系统以为是两次单击"。
 */
static void input_task(void *arg)
{
    (void)arg;

    bool last = (bsp_button_get_state() == BSP_BUTTON_PRESSED);
    bool ready_warned = false;

    /* 上电时 GPIO0 常被外部拉低（进入下载模式的那颗 strapping 脚），
       先等它稳定松开，避免一开机就给主机发一次"左键按下"，光标卡住。 */
    vTaskDelay(pdMS_TO_TICKS(120));
    last = (bsp_button_get_state() == BSP_BUTTON_PRESSED);

    while (1) {
        bool now = (bsp_button_get_state() == BSP_BUTTON_PRESSED);

        if (now != last) {
            last = now;

            if (!hid_device_is_ready()) {
                if (!ready_warned) {
                    ready_warned = true;
                    ui_show_hint("蓝牙未就绪，按键暂不可用");
                }
            } else {
                ready_warned = false;
                if (hid_mouse_button(HID_MOUSE_BTN_LEFT, now) == ESP_OK) {
                    ESP_LOGD(TAG, "mouse left button %s", now ? "down" : "up");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * 触摸任务
 *
 * 交互模型：
 *   - 屏幕本身就是触控板：任何模式下，**在空白处拖动 = 移动主机鼠标指针**
 *   - 遥控模式：屏幕上直接显示按钮，点按钮 = 执行对应动作
 *   - 鼠标模式：全屏纯触控板（右下角保留一个「遥控」小按钮，否则进去就出不来）
 *   - 触摸校准：只有主动点「校准」按钮才会进入，且向导自己独占触摸
 *
 * 区分「点击按钮」与「拖动指针」：按下时先做命中测试，
 * 命中按钮则本次触摸不产生指针移动；未命中则按触控板处理。
 *
 * ⚠️ 曾经有一个致命交互 bug：未校准时「任何一次触摸都把校准向导叫回来」，
 *    于是用户一碰屏幕，界面就跳到校准向导，所有按钮消失、鼠标也拖不动。
 *    现在校准只由「校准」按钮触发，不会再抢触摸。
 */
static void touch_task(void *arg)
{
    (void)arg;
    bsp_touch_data_t t;
    bool was_pressed = false;
    bool on_button = false;      /* 本次触摸是否落在按钮上 */
    ui_button_t pressed_btn = UI_BTN_NONE;
    int16_t last_x = 0, last_y = 0;
    uint32_t press_ms = 0;
    uint32_t moved = 0;

    while (1) {
        if (bsp_touch_read(&t) == ESP_OK) {
            /* ---- 校准向导独占触摸（仅在用户主动进入时） ---- */
            if (touch_cal_feed(t.pressed, t.raw_x, t.raw_y)) {
                touch_cal_sync_ui();

                touch_cal_stage_t st = touch_cal_stage();
                int tx, ty, px, py;
                bool has;
                touch_cal_target(st, &tx, &ty);
                touch_cal_last_point(&px, &py, &has);

                ui_touch_cal_state_t s = {
                    .stage    = (st == TOUCH_CAL_VERIFY) ? 3 : (int)st - 1,
                    .target_x = tx,
                    .target_y = ty,
                    .point_x  = px,
                    .point_y  = py,
                    .has_point = has && t.pressed,
                };
                ui_touch_cal_render(&s);
                was_pressed = t.pressed;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }

            /* ---- 正常交互：原始坐标 -> 屏幕坐标 ---- */
            uint16_t sx = 0, sy = 0;
            touch_cal_map(t.raw_x, t.raw_y, &sx, &sy);
            uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

            if (t.pressed) {
                if (!was_pressed) {
                    /* 触摸开始：判定是否落在按钮上 */
                    pressed_btn = ui_hit_test(sx, sy);
                    on_button = (pressed_btn != UI_BTN_NONE);
                    last_x = (int16_t)sx;
                    last_y = (int16_t)sy;
                    press_ms = now;
                    moved = 0;
#if TOUCH_DEBUG
                    ESP_LOGI(TAG, "touch raw=(%u,%u) -> (%u,%u) btn=%d",
                             t.raw_x, t.raw_y, sx, sy, (int)pressed_btn);
#endif
                } else if (!on_button) {
                    /* 拖动中（仅空白区）：移动主机指针 */
                    int16_t dx = (int16_t)(sx - last_x);
                    int16_t dy = (int16_t)(sy - last_y);
                    if (dx || dy) {
                        moved += (uint32_t)((dx > 0 ? dx : -dx) + (dy > 0 ? dy : -dy));
                        if (hid_device_is_ready()) {
                            int8_t mdx = (dx > 127) ? 127 : ((dx < -127) ? -127 : (int8_t)dx);
                            int8_t mdy = (dy > 127) ? 127 : ((dy < -127) ? -127 : (int8_t)dy);
                            hid_mouse_move(mdx, mdy);
                            pointer_track(mdx, mdy);
                        }
                    }
                    last_x = (int16_t)sx;
                    last_y = (int16_t)sy;
                }
                was_pressed = true;
            } else if (was_pressed) {
                /* 触摸结束 */
                was_pressed = false;

                if (on_button && pressed_btn != UI_BTN_NONE) {
                    /* 「鼠标」按钮长按 = 触摸校准：触摸不正常时这是唯一
                       还能进校准的入口，所以绑在一个大按钮上而不是藏菜单 */
                    if (pressed_btn == UI_BTN_MOUSE_MODE &&
                        moved < 12 && (now - press_ms) >= 1500) {
                        start_touch_cal();
                    } else {
                        /* 点击了按钮：执行对应动作 */
                        on_ui_button(pressed_btn, NULL);
                    }
                } else if (s_mouse_mode && moved < 12 && (now - press_ms) < 400) {
                    /* 鼠标模式下轻点空白处 = 左键单击 */
                    perform_action(ACTION_CLICK_LEFT);
                }
                on_button = false;
                pressed_btn = UI_BTN_NONE;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
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
        hid_device_service();   /* 广播看门狗 + 配对超时保护 */

        ui_conn_state_t st;
        switch (hid_device_state()) {
        case HID_STATE_READY:      st = UI_CONN_CONNECTED;    break;
        case HID_STATE_CONNECTING: st = UI_CONN_PAIRING;      break;
        default:                   st = UI_CONN_DISCONNECTED; break;
        }

        if (st != last) {
            last = st;
            ui_set_conn_state(st);
            /* 触摸校准向导正在跑时不要抢屏上的文字 */
            if (touch_cal_is_active()) {
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            switch (st) {
            case UI_CONN_CONNECTED:
                ui_show_action("已连接", "点屏幕按钮即可操作");
                ui_set_footer("实体键 = 鼠标左键");
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
                /* 配对失败往往是因为电脑端存着一把旧密钥，
                   这种提示能直接把用户引到正确的操作上 */
                if (hid_device_pairing_failed()) {
                    ui_show_action("配对失败", "电脑上移除设备后重试");
                    ui_set_footer("蓝牙设置里忽略 BiliMouse");
                } else {
                    ui_show_action("未连接", "请先配对蓝牙");
                    ui_set_footer("设备名 BiliMouse");
                }
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ==================== 入口 ==================== */

/**
 * NVS 必须最先初始化。
 *
 * 之前 settings_load() 跑在 hid_device_init() 之前，而初始化 NVS 的代码
 * 藏在 hid_device_init() 里面，于是启动日志必然出现
 *   E settings: nvs open failed: ESP_ERR_NVS_NOT_INITIALIZED
 * 后果不只是「读不到配置」：校准结果每次上电都会丢，
 * 且配对密钥的持久化也依赖 NVS 已经就绪。
 */
static esp_err_t app_nvs_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs full or version changed, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

void app_main(void)
{
    ESP_LOGI(TAG, "BiliMouse starting...");

    esp_err_t nvs_err = app_nvs_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s (配置与配对信息将无法保存)",
                 esp_err_to_name(nvs_err));
    }

    settings_load(&s_cfg);
    touch_cal_init(on_touch_cal_done);
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

    /* 体感（甩腕/翻转）已停用：躺在床上翻身、拿放设备都会误触发，
       实测「暂停/播放」被打断的次数远多于主动使用的次数。
       IMU 仍然初始化，后续要恢复体感功能时只需把 imu_task 加回来。 */

    ESP_LOGI(TAG, "startup complete");
}
