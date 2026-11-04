#include "touch_cal.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "settings.h"

static const char *TAG = "touch_cal";

/* 三个目标点（屏幕坐标）。避开顶部信息卡与底部提示条，躺着也能点准 */
#define P1_X  40
#define P1_Y  124
#define P2_X  280
#define P2_Y  124
#define P3_X  40
#define P3_Y  196

#define SCR_W 320
#define SCR_H 240

/* 每步最多等这么久，超时说明触摸根本没反应，不能把用户困在向导里 */
#define STEP_TIMEOUT_MS  60000
/* 验证阶段持续多久 */
#define VERIFY_MS        6000

/* 至少按下这么久才算一次有效点击（滤掉抖动） */
#define PRESS_MIN_MS     30

static float s_k[6];              /* 仿射矩阵 */
static bool  s_calibrated = false;

static touch_cal_stage_t s_stage = TOUCH_CAL_IDLE;
static touch_cal_done_cb_t s_done_cb = NULL;

static uint16_t s_raw[3][2];      /* 三个采样点的原始坐标 */
static uint8_t  s_n;              /* 已采集点数 */
static uint8_t  s_fail;           /* 解算失败次数 */
static uint32_t s_stage_ms;       /* 本阶段开始时间 */
static bool     s_was_pressed;
static bool     s_ignore_release; /* 启动向导的那一次按压不算采样 */
static uint32_t s_press_ms;

static int s_last_x = -1;
static int s_last_y = -1;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ==================== 映射 ==================== */

static inline uint16_t clamp_u16(int v, int max)
{
    if (v < 0) {
        return 0;
    }
    if (v > max) {
        return (uint16_t)max;
    }
    return (uint16_t)v;
}

void touch_cal_map(uint16_t raw_x, uint16_t raw_y, uint16_t *sx, uint16_t *sy)
{
    if (s_calibrated) {
        float x = s_k[0] * raw_x + s_k[1] * raw_y + s_k[2];
        float y = s_k[3] * raw_x + s_k[4] * raw_y + s_k[5];
        *sx = clamp_u16((int)lroundf(x), SCR_W - 1);
        *sy = clamp_u16((int)lroundf(y), SCR_H - 1);
        return;
    }

    /* 未校准的兜底映射。
       实测本板 FT6336 输出的是**像素坐标**：
         raw_x ∈ [0,239]  → 面板竖屏的"宽"（对应屏幕高度）
         raw_y ∈ [0,319]  → 面板竖屏的"高"（对应屏幕宽度）
       屏幕是 320x240 横屏，所以是 90° 旋转：
         screen_x = raw_y
         screen_y = 239 - raw_x
       方向是否镜像因装配而异，所以仍然建议做一次三点校准。 */
    if (raw_x <= 300 && raw_y <= 340) {
        *sx = clamp_u16((int)raw_y, SCR_W - 1);
        *sy = clamp_u16((int)(SCR_H - 1) - (int)raw_x, SCR_H - 1);
        return;
    }

    /* 少数批次输出 12bit（0..4095），按比例换算后再做同样的旋转 */
    int px = (int)((uint32_t)raw_x * 239 / 4095);
    int py = (int)((uint32_t)raw_y * 319 / 4095);
    *sx = clamp_u16(py, SCR_W - 1);
    *sy = clamp_u16((int)(SCR_H - 1) - px, SCR_H - 1);
}

bool touch_cal_is_calibrated(void)
{
    return s_calibrated;
}

/* ==================== 向导 ==================== */

void touch_cal_init(touch_cal_done_cb_t cb)
{
    s_done_cb = cb;
    s_calibrated = false;

    const settings_t *cfg = settings_get();
    if (cfg != NULL && cfg->touch_calibrated) {
        memcpy(s_k, cfg->touch_k, sizeof(s_k));
        s_calibrated = true;
        ESP_LOGI(TAG, "loaded mapping: [%.4f %.4f %.2f] [%.4f %.4f %.2f]",
                 s_k[0], s_k[1], s_k[2], s_k[3], s_k[4], s_k[5]);
    } else {
        ESP_LOGW(TAG, "no touch calibration yet");
    }
}

bool touch_cal_is_active(void)
{
    return s_stage != TOUCH_CAL_IDLE;
}

touch_cal_stage_t touch_cal_stage(void)
{
    return s_stage;
}

void touch_cal_start(void)
{
    touch_cal_start_ex(false);
}

void touch_cal_start_ex(bool pressed_now)
{
    s_stage = TOUCH_CAL_TAP1;
    s_n = 0;
    s_fail = 0;
    s_was_pressed = pressed_now;
    s_ignore_release = pressed_now;
    s_stage_ms = now_ms();
    s_last_x = s_last_y = -1;
    ESP_LOGI(TAG, "touch calibration started (pressed_now=%d), tap point 1/3", pressed_now);
}

void touch_cal_cancel(void)
{
    if (s_stage != TOUCH_CAL_IDLE) {
        ESP_LOGW(TAG, "touch calibration cancelled");
        s_stage = TOUCH_CAL_IDLE;
        if (s_done_cb) {
            s_done_cb(false);
        }
    }
}

void touch_cal_target(touch_cal_stage_t stage, int *x, int *y)
{
    switch (stage) {
    case TOUCH_CAL_TAP1: *x = P1_X; *y = P1_Y; break;
    case TOUCH_CAL_TAP2: *x = P2_X; *y = P2_Y; break;
    case TOUCH_CAL_TAP3: *x = P3_X; *y = P3_Y; break;
    default:             *x = SCR_W / 2; *y = SCR_H / 2; break;
    }
}

void touch_cal_last_point(int *x, int *y, bool *valid)
{
    *x = s_last_x;
    *y = s_last_y;
    *valid = (s_last_x >= 0);
}

/**
 * 用 3 个点解仿射矩阵。
 *
 * 屏幕坐标 P1(左上) P2(右上) P3(左下)，对应原始坐标 R1 R2 R3。
 * 令 u = R2-R1（屏幕 X 方向在面板上的向量）、v = R3-R1（屏幕 Y 方向），
 * 则  raw = R1 + ((sx-P1x)/(P2x-P1x))*u + ((sy-P1y)/(P3y-P1y))*v
 * 反解即得 screen = f(raw)。
 */
static bool solve(void)
{
    float r1x = s_raw[0][0], r1y = s_raw[0][1];
    float ux = (float)s_raw[1][0] - r1x;
    float uy = (float)s_raw[1][1] - r1y;
    float vx = (float)s_raw[2][0] - r1x;
    float vy = (float)s_raw[2][1] - r1y;

    float det = ux * vy - vx * uy;
    ESP_LOGI(TAG, "solve: u=(%.1f,%.1f) v=(%.1f,%.1f) det=%.1f", ux, uy, vx, vy, det);

    /* 三点几乎共线 -> 采集无效（用户点得离目标太远或面板异常） */
    if (fabsf(det) < 500.0f) {
        ESP_LOGE(TAG, "degenerate calibration (det too small), please retry");
        return false;
    }

    const float w = (float)(P2_X - P1_X);
    const float h = (float)(P3_Y - P1_Y);

    s_k[0] = w * vy / det;
    s_k[1] = -w * vx / det;
    s_k[2] = P1_X - s_k[0] * r1x - s_k[1] * r1y;

    s_k[3] = -h * uy / det;
    s_k[4] = h * ux / det;
    s_k[5] = P1_Y - s_k[3] * r1x - s_k[4] * r1y;

    ESP_LOGI(TAG, "mapping: [%.4f %.4f %.2f] [%.4f %.4f %.2f]",
             s_k[0], s_k[1], s_k[2], s_k[3], s_k[4], s_k[5]);

    /* 合理性检查：把三个采样点代回去，误差应远小于 1 像素 */
    for (int i = 0; i < 3; i++) {
        int x, y;
        touch_cal_target((touch_cal_stage_t)(TOUCH_CAL_TAP1 + i), &x, &y);
        float mx = s_k[0] * s_raw[i][0] + s_k[1] * s_raw[i][1] + s_k[2];
        float my = s_k[3] * s_raw[i][0] + s_k[4] * s_raw[i][1] + s_k[5];
        if (fabsf(mx - x) > 2.0f || fabsf(my - y) > 2.0f) {
            ESP_LOGE(TAG, "self-check failed on point %d (%.1f,%.1f) vs (%d,%d)",
                     i + 1, mx, my, x, y);
            return false;
        }
    }

    return true;
}

static void finish(bool ok)
{
    s_stage = TOUCH_CAL_IDLE;
    s_calibrated = ok;
    settings_t *cfg = settings_mut();
    if (cfg != NULL) {
        cfg->touch_calibrated = ok;
        if (ok) {
            memcpy(cfg->touch_k, s_k, sizeof(s_k));
        }
        settings_save(cfg);
    }

    ESP_LOGW(TAG, "touch calibration %s", ok ? "saved" : "failed");
    if (s_done_cb) {
        s_done_cb(ok);
    }
}

bool touch_cal_feed(bool pressed, uint16_t raw_x, uint16_t raw_y)
{
    if (s_stage == TOUCH_CAL_IDLE) {
        return false;
    }

    uint32_t now = now_ms();

    /* 验证阶段：只更新十字位置，到时间自动结束 */
    if (s_stage == TOUCH_CAL_VERIFY) {
        if (pressed) {
            uint16_t sx, sy;
            touch_cal_map(raw_x, raw_y, &sx, &sy);
            s_last_x = sx;
            s_last_y = sy;
        }
        if (now - s_stage_ms > VERIFY_MS) {
            finish(true);
        }
        s_was_pressed = pressed;
        return true;
    }

    /* 任何一个采集步骤超时 -> 放弃，避免把设备锁死在向导里 */
    if (now - s_stage_ms > STEP_TIMEOUT_MS) {
        ESP_LOGE(TAG, "touch not responding during calibration, aborting");
        finish(false);
        return true;
    }

    bool pressed_edge = pressed && !s_was_pressed;
    bool release_edge = !pressed && s_was_pressed;

    /* 启动向导的那一次触摸整体丢弃 */
    if (s_ignore_release) {
        if (!pressed) {
            s_ignore_release = false;
        }
        s_was_pressed = pressed;
        return true;
    }

    if (pressed_edge) {
        s_press_ms = now;
    }

    if (release_edge && (now - s_press_ms) >= PRESS_MIN_MS) {
        s_raw[s_n][0] = raw_x;
        s_raw[s_n][1] = raw_y;
        ESP_LOGI(TAG, "point %d recorded: raw=(%u,%u)", s_n + 1, raw_x, raw_y);
        s_n++;

        if (s_n >= 3) {
            if (solve()) {
                /* 立刻生效：验证阶段要用新映射画十字 */
                s_calibrated = true;
                s_stage = TOUCH_CAL_VERIFY;
                s_stage_ms = now;
            } else {
                /* 重来一遍；连续两次解不出来说明触摸数据本身有问题，直接退出 */
                s_n = 0;
                s_fail++;
                if (s_fail >= 2) {
                    ESP_LOGE(TAG, "calibration failed twice, giving up");
                    finish(false);
                    return true;
                }
                s_stage = TOUCH_CAL_TAP1;
                s_stage_ms = now;
            }
        } else {
            s_stage = (touch_cal_stage_t)(TOUCH_CAL_TAP1 + s_n);
            s_stage_ms = now;
        }
    }

    if (pressed) {
        uint16_t sx, sy;
        touch_cal_map(raw_x, raw_y, &sx, &sy);
        s_last_x = sx;
        s_last_y = sy;
    }

    s_was_pressed = pressed;
    return true;
}
