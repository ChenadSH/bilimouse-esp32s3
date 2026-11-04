#include "button_fsm.h"

#include <string.h>

/* 内部状态 */
typedef enum {
    ST_IDLE = 0,       /* 空闲，等待按下 */
    ST_PRESSED,        /* 已按下，判定中 */
    ST_CLICK_PENDING,  /* 单击候选，等待第二击窗口 */
} fsm_state_t;

static fsm_config_t s_cfg;
static gesture_cb_t s_cb;
static void *s_ctx;

static fsm_state_t s_state = ST_IDLE;
static uint32_t    s_press_time;      /* 按下时刻 */
static bool        s_long_fired;       /* 长按已触发 */
static bool        s_super_fired;      /* 超长按已触发 */
static bool        s_second_click;     /* 当前这次按下是第二击 */
static bool        s_first_click_valid;/* 第一击是有效短按 */
static uint32_t    s_click_deadline;   /* 单击候选截止时刻 */

static void fire(gesture_t g)
{
    if (s_cb && g != GESTURE_NONE) {
        s_cb(g, s_ctx);
    }
}

void button_fsm_init(const fsm_config_t *cfg, gesture_cb_t cb, void *ctx)
{
    if (cfg) {
        s_cfg = *cfg;
    } else {
        s_cfg.click_max_ms        = 250;
        s_cfg.double_click_gap_ms = 300;
        s_cfg.long_press_ms       = 600;
        s_cfg.super_long_press_ms = 1500;
    }
    s_cb  = cb;
    s_ctx = ctx;

    s_state             = ST_IDLE;
    s_press_time        = 0;
    s_long_fired        = false;
    s_super_fired       = false;
    s_second_click      = false;
    s_first_click_valid = false;
    s_click_deadline    = 0;
}

void button_fsm_update(bool pressed, uint32_t now_ms)
{
    if (pressed) {
        switch (s_state) {
        case ST_IDLE:
            s_state          = ST_PRESSED;
            s_press_time     = now_ms;
            s_long_fired     = false;
            s_super_fired    = false;
            s_second_click   = false;
            break;

        case ST_CLICK_PENDING:
            /* 第二击到来 -> 判定为双击，撤销待执行的单击 */
            s_second_click      = true;
            s_first_click_valid = false;
            s_state             = ST_PRESSED;
            s_press_time        = now_ms;
            s_long_fired        = false;
            s_super_fired       = false;
            break;

        case ST_PRESSED:
        default:
            /* 重复的按下事件，忽略 */
            break;
        }
        return;
    }

    /* ---- 松开 ---- */
    if (s_state != ST_PRESSED) {
        return;
    }

    uint32_t held = now_ms - s_press_time;

    if (s_super_fired || s_long_fired) {
        /* 长按/超长按已在按住期间触发过，松开不再产生动作 */
        s_state = ST_IDLE;
        s_second_click = false;
        return;
    }

    if (held > s_cfg.click_max_ms) {
        /* 按住过久但未达长按阈值：本次按压无效。
           若第一击有效，则补发单击（第二击超时的情况）。 */
        s_state = ST_IDLE;
        if (s_second_click && s_first_click_valid) {
            s_second_click      = false;
            s_first_click_valid = false;
            fire(GESTURE_SINGLE_CLICK);
        }
        s_second_click = false;
        return;
    }

    /* 短按 */
    if (s_second_click) {
        /* 第二击的松开 -> 双击完成 */
        s_state             = ST_IDLE;
        s_second_click      = false;
        s_first_click_valid = false;
        fire(GESTURE_DOUBLE_CLICK);
        return;
    }

    /* 第一击的松开 -> 进入单击候选窗口 */
    s_state             = ST_CLICK_PENDING;
    s_first_click_valid = true;
    s_click_deadline    = now_ms + s_cfg.double_click_gap_ms;
}

void button_fsm_tick(uint32_t now_ms)
{
    switch (s_state) {
    case ST_PRESSED: {
        uint32_t held = now_ms - s_press_time;

        if (!s_super_fired && held >= s_cfg.super_long_press_ms) {
            s_super_fired = true;
            s_long_fired  = true;   /* 超长按覆盖长按 */
            fire(GESTURE_SUPER_LONG);
        } else if (!s_long_fired && held >= s_cfg.long_press_ms) {
            s_long_fired = true;
            fire(GESTURE_LONG_PRESS);
        }
        break;
    }

    case ST_CLICK_PENDING:
        if (now_ms >= s_click_deadline) {
            s_state             = ST_IDLE;
            s_first_click_valid = false;
            fire(GESTURE_SINGLE_CLICK);
        }
        break;

    case ST_IDLE:
    default:
        break;
    }
}
