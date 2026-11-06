/**
 * @file app_main.c
 * @brief BiliMouse 主程序
 *
 * 任务编排：
 *   ui_task      - 界面刷新（约 20fps，息屏时暂停）
 *   input_task   - 实体按键 -> 空格键（播放/暂停）
 *   touch_task   - 触摸：拖动 = 移动指针；轻点 = 左键；点按钮 = 执行动作
 *   hid_monitor_task - 蓝牙连接状态同步
 *   power_task   - 空闲变暗 / 息屏
 *
 * 交互模型（第六版）：
 *   1. 实体按键 = 空格。B 站网页播放器把空格绑定为播放/暂停，
 *      比 Ctrl+左键 模拟右键可靠得多，也不依赖 macOS 的鼠标按键支持。
 *   2. 鼠标模式：屏幕左右两侧各一个「左键 / 右键」竖长按键，
 *      按下即按住、抬起才松开，中间区域是触控板，可按住拖拽。
 *   3. 遥控模式：三行大按钮 + 次行的「红外」入口。
 *   4. 屏幕轻点 = 左键单击；快速点两下 = 双击（交给主机系统判定）。
 *   5. 红外：学习任意遥控器的原始波形，存在 NVS 里，一键回放。
 *   6. 省电：30s 无操作变暗，90s 息屏，触摸即唤醒。
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "actions.h"
#include "bsp_button.h"
#include "bsp_imu.h"
#include "bsp_touch.h"
#include "calibrate.h"
#include "cli.h"
#include "hid_device.h"
#include "ir_remote.h"
#include "ota_update.h"
#include "pointer.h"
#include "power_mgr.h"
#include "radio.h"
#include "settings.h"
#include "touch_cal.h"
#include "ui.h"

static const char *TAG = "bilimouse";

/* 触摸排查开关：打开后每次按下会打印一次原始/映射坐标 */
#define TOUCH_DEBUG 1

/**
 * 触控板增益（自适应两段式）：
 *   - 单次采样位移 <= TOUCH_GAIN_FINE_MAX 像素（手在慢慢微调）-> 1:1，
 *     保证光标跟手指一样精细，能点到小目标；
 *   - 超过阈值（快速滑动）-> TOUCH_GAIN_FAST 倍，屏幕小也能快速横跨大屏。
 * 之前固定 3 倍的问题：微调时手抖 1 像素光标跳 3 像素，根本对不准。
 */
#define TOUCH_GAIN_FINE_MAX 2
#define TOUCH_GAIN_FAST     2

static settings_t s_cfg;
static bool s_mouse_mode = false;
static int  s_cal_shown_stage = -1;   /* 触摸校准界面当前展示的阶段 */

/* 前向声明 */
static void perform_action(action_t action);
static void on_ui_button(ui_button_t btn, void *ctx);
static void start_touch_cal(void);
static void set_mouse_mode(bool on);
static void enter_ir_mode(void);

/* ==================== 模式切换 ==================== */

static void set_mouse_mode(bool on)
{
    s_mouse_mode = on;
    hid_mouse_release_all();   /* 切模式时别把主机卡在"按住"状态 */
    if (on) {
        ui_set_mode(UI_MODE_MOUSE);
        ui_show_action("鼠标模式", "两侧=左右键");
        ui_set_footer("中间拖动=移动  轻点=左键");
    } else {
        ui_set_mode(UI_MODE_HOME);
        ui_show_action("BiliMouse", "左右滑动选功能");
        ui_set_footer("左右滑动选功能，点图标打开");
    }
}

/* ---- 首页与各功能页 ---- */

static void go_home(void)
{
    set_mouse_mode(false);
}

static void enter_ir_mode(void);   /* 定义在红外遥控小节 */

static void enter_radio(void)
{
    ui_set_mode(UI_MODE_RADIO);
    radio_enter();
}

static void enter_ota_page(void)
{
    ui_set_mode(UI_MODE_OTA_PAGE);
    ui_show_action("在线升级", "点「开始检查更新」");
    ui_set_footer("升级中全程别断电");
}

/** 首页当前 icon 进入对应功能页 */
static void open_home_icon(home_icon_t icon)
{
    switch (icon) {
    case HOME_ICON_BILI:
        ui_set_mode(UI_MODE_REMOTE);
        ui_show_action("B站控制", "点屏幕按钮操作");
        ui_set_footer("实体键=空格  轻点=左键");
        break;
    case HOME_ICON_MOUSE:
        set_mouse_mode(true);
        break;
    case HOME_ICON_IR:
        enter_ir_mode();
        break;
    case HOME_ICON_RADIO:
        enter_radio();
        break;
    case HOME_ICON_OTA:
        enter_ota_page();
        break;
    case HOME_ICON_CAL:
        start_touch_cal();
        break;
    default:
        break;
    }
}

static volatile bool s_ir_learning;

/** 刷新红外页面的卡片文字（槽位 / 是否已学） */
static void ir_refresh(const char *title)
{
    uint8_t slot = ir_remote_active_slot();
    char sub[48];
    snprintf(sub, sizeof(sub), "槽 %u/%u %s 已学 %u 个",
             (unsigned)(slot + 1), (unsigned)IR_SLOT_COUNT,
             ir_remote_slot_loaded(slot) ? "有码" : "空",
             (unsigned)ir_remote_loaded_count());
    ui_show_action(title, sub);
}

/**
 * 学习任务：捕获是阻塞的（最长 8 秒），必须放到独立任务里，
 * 否则触摸任务会跟着卡住，连「返回」都点不动。
 */
static void ir_learn_task(void *arg)
{
    (void)arg;
    s_ir_learning = true;
    ir_refresh("学习中");
    ui_set_footer("对准设备，按一下遥控器");

    esp_err_t e = ir_remote_learn(8000);

    s_ir_learning = false;
    if (e == ESP_OK) {
        ir_refresh("已学会");
        ui_set_footer("已写入 flash，重启/重烧固件都不丢");
    } else {
        ir_refresh("没收到信号");
        ui_set_footer("靠近一点，确认接收头接 GPIO11");
    }
    vTaskDelete(NULL);
}

static void enter_ir_mode(void)
{
    hid_mouse_release_all();
    ui_set_mode(UI_MODE_IR);
    ir_refresh("红外遥控");
    if (!ir_remote_ready()) {
        ui_set_footer("红外不可用：检查 GPIO10/11");
    } else {
        ui_set_footer("学习=复制遥控器  发射=回放");
    }
}

/* ==================== 屏幕按钮回调 ==================== */

static void on_ui_button(ui_button_t btn, void *ctx)
{
    (void)ctx;

    switch (btn) {
    /* ---- 首页 ---- */
    case UI_BTN_HOME_ICON0:
        open_home_icon(ui_home_current_icon());
        break;
    case UI_BTN_HOME_PREV:
        ui_home_prev_page();
        break;
    case UI_BTN_HOME_NEXT:
        ui_home_next_page();
        break;
    case UI_BTN_HOME:
        radio_stop();      /* 离开电台页时顺手停播，静默无副作用 */
        go_home();
        break;

    /* ---- B站播放控制页 ---- */
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
    case UI_BTN_OTA_START:
        enter_ota_page();
        /* 两段式：先只做「检查」（WiFi/清单/版本 三步逐步显示），
           检测到新版本后屏上才会出现「立即升级」按钮 */
        ota_update_check();
        break;
    case UI_BTN_OTA_DOWNLOAD:
        ota_update_download();
        break;

    /* 电台模式 */
    case UI_BTN_RADIO_PREV:
        radio_prev();
        break;
    case UI_BTN_RADIO_NEXT:
        radio_next();
        break;
    case UI_BTN_RADIO_TOGGLE:
        radio_toggle();
        break;
    case UI_BTN_RADIO_LIST:
        ui_set_mode(UI_MODE_RADIO_LIST);
        ui_show_action("选择电台", "点台名直接播放");
        break;
    case UI_BTN_RADIO_LIST_BACK:
        ui_set_mode(UI_MODE_RADIO);
        ui_show_action("电台", "已返回");
        break;

    /* 台列表点行选台（UI_BTN_RADIO_PICK0..7） */
    case UI_BTN_RADIO_PICK0:
    case UI_BTN_RADIO_PICK1:
    case UI_BTN_RADIO_PICK2:
    case UI_BTN_RADIO_PICK3:
    case UI_BTN_RADIO_PICK4:
    case UI_BTN_RADIO_PICK5:
    case UI_BTN_RADIO_PICK6:
    case UI_BTN_RADIO_PICK7:
        radio_select(btn - UI_BTN_RADIO_PICK0);
        ui_set_mode(UI_MODE_RADIO);
        break;

    /* 红外模式 */
    case UI_BTN_IR_LEARN:
        if (!ir_remote_ready()) {
            ui_show_action("红外不可用", "检查 GPIO10/11");
        } else if (!s_ir_learning) {
            xTaskCreate(ir_learn_task, "ir_learn", 4096, NULL, 3, NULL);
        }
        break;

    case UI_BTN_IR_SEND: {
        esp_err_t e = ir_remote_send();
        if (e == ESP_OK) {
            ir_refresh("已发射");
        } else if (e == ESP_ERR_NOT_FOUND) {
            ir_refresh("这个槽是空的");
            ui_set_footer("先点「学习」录一个码");
        } else {
            ir_refresh("发射失败");
        }
        break;
    }

    case UI_BTN_IR_SLOT:
        ir_remote_next_slot();
        ir_refresh("切换槽位");
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
    ui_set_mode(s_mouse_mode ? UI_MODE_MOUSE : UI_MODE_HOME);
    s_cal_shown_stage = -1;

    if (ok) {
        ui_show_action("触摸已校准", "点按钮试试");
        ui_set_footer("左右滑动选功能，点图标打开");
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
 * 实体按键任务：空格 = 播放/暂停。
 *
 * 为什么从「右键」改成空格：
 *   1. B 站网页播放器把空格绑定为播放/暂停，而且页面级生效，
 *      不要求焦点落在播放器上；
 *   2. 之前的 Ctrl+左键 方案依赖 macOS 把修饰键和鼠标点击串起来判右键，
 *      在触控板拖动、输入框聚焦等场景下行为不稳定；
 *   3. 键盘报告只有一个字节的修饰位，没有"按住状态没清干净"的副作用。
 *
 * 按下即触发（而不是松开触发）：躺着单手按，按下就有反馈最直觉。
 * 长按不重复触发，避免主机键盘自动重复导致播放/暂停反复横跳。
 */
static void input_task(void *arg)
{
    (void)arg;

    bool last = (bsp_button_get_state() == BSP_BUTTON_PRESSED);
    bool ready_warned = false;
    uint32_t press_ms = 0;
    bool reset_warned = false;

    /* 上电时 GPIO0 常被外部拉低（进入下载模式的那颗 strapping 脚），
       先等它稳定松开，避免一开机就给主机发一次空格。 */
    vTaskDelay(pdMS_TO_TICKS(120));
    last = (bsp_button_get_state() == BSP_BUTTON_PRESSED);

    while (1) {
        bool now = (bsp_button_get_state() == BSP_BUTTON_PRESSED);
        uint32_t tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        if (now != last) {
            last = now;
            if (now) {
                press_ms = tick;
                reset_warned = false;

                if (!hid_device_is_ready()) {
                    if (!ready_warned) {
                        ready_warned = true;
                        ui_show_hint("蓝牙未就绪，按键暂不可用");
                    }
                } else {
                    ready_warned = false;
                    power_mgr_notify_activity();
                    esp_err_t e = hid_keyboard_tap(HID_KEY_SPACEBAR, HID_MOD_NONE);
                    ESP_LOGI(TAG, "button: space tap (%s)", esp_err_to_name(e));
                    ui_show_hint(e == ESP_OK ? "空格：播放/暂停" : "发送失败");
                }
            }
        } else if (now) {
            /* 长按 2s 提示、5s 执行：清空本机所有配对密钥后重启，
               重新以全新身份广播。主机端怎么删记录都追得上 —— 这是
               "搜索不到设备" 时的最后一招自救，不需要接电脑。 */
            uint32_t held = tick - press_ms;
            if (!reset_warned && held >= 2000) {
                reset_warned = true;
                ui_show_hint("继续按住 = 重置蓝牙配对");
            }
            if (held >= 5000) {
                ESP_LOGW(TAG, "long press: clearing bonds and restarting");
                ui_show_hint("清除配对，正在重启");
                hid_device_clear_bonds();
                vTaskDelay(pdMS_TO_TICKS(300));
                esp_restart();
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
    bool swallow = false;        /* 本次触摸只用来点亮屏幕，不执行动作 */
    bool home_gesture = false;   /* 首页空白处滑动 = 翻页，不移动指针 */
    int16_t first_x = 0;         /* 按下时的起始 x（计算滑动方向） */
    ui_button_t pressed_btn = UI_BTN_NONE;
    uint8_t hold_btn = 0;        /* 当前按住的鼠标键（左/右），0=没按住 */
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
                    /* 息屏状态下，这一下触摸只负责点亮屏幕 —— 否则用户
                       摸黑想找按钮，结果是直接把视频暂停了。 */
                    power_mgr_notify_activity();
                    if (!power_mgr_display_on()) {
                        swallow = true;
                        was_pressed = true;
                        vTaskDelay(pdMS_TO_TICKS(20));
                        continue;
                    }

                    /* 触摸开始：判定是否落在按钮上 */
                    pressed_btn = ui_hit_test(sx, sy);
                    on_button = (pressed_btn != UI_BTN_NONE);
                    home_gesture = ui_mode_is_home() && !on_button;
                    first_x = (int16_t)sx;
                    last_x = (int16_t)sx;
                    last_y = (int16_t)sy;
                    press_ms = now;
                    moved = 0;

                    /* 左右键是「按住型」：按下立即发 down，抬起才发 up。
                       这样按住后往中间拖 = 拖拽/框选，和真实鼠标一致。 */
                    if (pressed_btn == UI_BTN_MOUSE_LEFT) {
                        hold_btn = HID_MOUSE_BTN_LEFT;
                        hid_mouse_button(hold_btn, true);
                        ui_show_hint("左键按住中");
                    } else if (pressed_btn == UI_BTN_MOUSE_RIGHT) {
                        hold_btn = HID_MOUSE_BTN_RIGHT;
                        hid_mouse_button(hold_btn, true);
                        ui_show_hint("右键按住中");
                    }
#if TOUCH_DEBUG
                    ESP_LOGI(TAG, "touch raw=(%u,%u) -> (%u,%u) btn=%d",
                             t.raw_x, t.raw_y, sx, sy, (int)pressed_btn);
#endif
                } else if (home_gesture) {
                    /* 首页空白滑动：只累计距离，翻页在松手时判定 */
                    int32_t hdx = (int32_t)(sx - last_x);
                    int32_t hdy = (int32_t)(sy - last_y);
                    moved += (uint32_t)((hdx > 0 ? hdx : -hdx) +
                                        (hdy > 0 ? hdy : -hdy));
                    last_x = (int16_t)sx;
                    last_y = (int16_t)sy;
                } else if (!on_button || hold_btn) {
                    /* 拖动（空白区，或按住左右键后拖出按钮）。
                       增益自适应：慢慢微调时 1:1，快速滑动时放大。
                       乘完增益可能超过 int8 上限(127)，按 ±127 分包连续发送。 */
                    int32_t dx = (int32_t)(sx - last_x);
                    int32_t dy = (int32_t)(sy - last_y);
                    int32_t mag = (dx > 0 ? dx : -dx) + (dy > 0 ? dy : -dy);
                    if (dx || dy) {
                        /* ⚠️ moved 必须累计原始像素，不能累计乘过增益的位移：
                           上一版在这里累加了 3 倍值，手指 4 像素的抖动就把
                           moved 顶过阈值，轻点被误判成拖动 -> 左键失效。 */
                        moved += (uint32_t)mag;
                        if (hid_device_is_ready()) {
                            int32_t gain = (mag <= TOUCH_GAIN_FINE_MAX) ? 1 : TOUCH_GAIN_FAST;
                            int32_t fx = dx * gain;
                            int32_t fy = dy * gain;
                            while (fx || fy) {
                                int8_t mdx = 0, mdy = 0;
                                if (fx > 0)      { mdx = (fx >  127) ?  127 : (int8_t)fx; }
                                else if (fx < 0) { mdx = (fx < -127) ? -127 : (int8_t)fx; }
                                if (fy > 0)      { mdy = (fy >  127) ?  127 : (int8_t)fy; }
                                else if (fy < 0) { mdy = (fy < -127) ? -127 : (int8_t)fy; }
                                hid_mouse_move(mdx, mdy);
                                pointer_track(mdx, mdy);
                                fx -= mdx;
                                fy -= mdy;
                            }
                        }
                    }
                    last_x = (int16_t)sx;
                    last_y = (int16_t)sy;
                }
                was_pressed = true;
            } else if (was_pressed) {
                /* 触摸结束 */
                was_pressed = false;

                if (swallow) {
                    swallow = false;
                } else if (home_gesture) {
                    /* 首页滑动翻页：水平位移超过 50px 算一次翻页。
                       手指左划（dx<0）= 下一页，右划 = 上一页。 */
                    int16_t dx = (int16_t)(last_x - first_x);
                    if (dx < -50) {
                        ui_home_next_page();
                    } else if (dx > 50) {
                        ui_home_prev_page();
                    }
                    home_gesture = false;
                } else if (hold_btn) {
                    /* 松开左右键 */
                    hid_mouse_button(hold_btn, false);
                    ui_show_hint(hold_btn == HID_MOUSE_BTN_LEFT ? "左键已松开" : "右键已松开");
                    hold_btn = 0;
                } else if (on_button && pressed_btn != UI_BTN_NONE) {
                    /* 首页 icon 长按 = 触摸校准：触摸漂移时按钮可能点不准，
                       这是常驻的校准入口，绑在首页大磁贴上而不是藏菜单 */
                    if (pressed_btn == UI_BTN_HOME_ICON0 &&
                        moved < 12 && (now - press_ms) >= 1500) {
                        start_touch_cal();
                    } else {
                        /* 点击了按钮：执行对应动作 */
                        on_ui_button(pressed_btn, NULL);
                    }
                } else if (moved < 12 && (now - press_ms) < 400) {
                    /* 轻点空白处 = 左键单击（鼠标/遥控模式都生效）。
                       快速点两下 = 双击：由主机在系统双击窗口内自动判定，
                       固件只管把两次干净的 down/up 发出去。 */
                    perform_action(ACTION_CLICK_LEFT);
                }
                on_button = false;
                pressed_btn = UI_BTN_NONE;
                home_gesture = false;
            }
        }
        /* 息屏后把采样降到 150ms：I2C 轮询是空闲时的主要耗电项，
           而唤醒只要一次触摸，慢一点完全无感。 */
        vTaskDelay(pdMS_TO_TICKS(power_mgr_display_on() ? 20 : 150));
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    while (1) {
        /* 息屏时完全不刷屏：省下 SPI 带宽和 CPU，也让 LEDC 背光保持 0 */
        if (power_mgr_display_on()) {
            ui_flush();
        }
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
                ui_set_footer("左右滑动选功能，点图标打开");
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
        /* 背光统一由 power_mgr 管理（读设置里的亮度 + 空闲自动变暗） */
        power_mgr_init();
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

    /* 红外收发（RMT）。接不接红外头都不影响其它功能，
       初始化失败只是让「红外」按钮报不可用。 */
    if (ir_remote_init() != ESP_OK) {
        ESP_LOGW(TAG, "ir init failed (红外功能不可用)");
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
    power_mgr_start();

    /* 串口命令行：红外码备份 + WiFi/OTA 配置（idf.py monitor 里敲 ir/wifi/ota） */
    cli_start();

    /* WiFi + OTA（失败不影响其它功能，只是「升级」按钮不可用）。
       esp_netif_init 在这里完成 lwip 初始化；任何依赖网络栈的模块
       （如电台）都必须在它之后启动，否则 socket() 会触发
       tcpip_send_msg_wait_sem "Invalid mbox" 断言 -> 开机重启循环。 */
    if (ota_update_init() != ESP_OK) {
        ESP_LOGW(TAG, "ota init failed (在线升级不可用)");
    }

    /* 走到这里 = 外设、蓝牙、UI 全部就绪：如果本次启动是 OTA 升级后的
       第一次启动（PENDING_VERIFY 状态），确认新固件有效。
       不确认的话，下次重启 bootloader 会认为新固件失败并回滚旧版本。 */
    esp_ota_mark_app_valid_cancel_rollback();

    /* 体感（甩腕/翻转）已停用：躺在床上翻身、拿放设备都会误触发，
       实测「暂停/播放」被打断的次数远多于主动使用的次数。
       IMU 仍然初始化，后续要恢复体感功能时只需把 imu_task 加回来。 */

    ESP_LOGI(TAG, "startup complete");
}
