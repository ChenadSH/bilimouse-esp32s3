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
static bool        s_hold_hint_sent;  /* 长按提示已发 */
static bool        s_super_hint_sent; /* 超长按提示已发 */
static bool        s_second_click;    /* 当前这次按下是第二击 */
static bool        s_first_click_valid;/* 第一击是有效短按 */
static uint32_t    s_click_deadline;  /* 单击候选截止时刻 */

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

    s_state              = ST_IDLE;
    s_press_time         = 0;
    s_hold_hint_sent     = false;
    s_super_hint_sent    = false;
    s_second_click       = false;
    s_first_click_valid  = false;
    s_click_deadline     = 0;
}

void button_fsm_update(bool pressed, uint32_t now_ms)
{
    if (pressed) {
        switch (s_state) {
        case ST_IDLE:
            s_state           = ST_PRESSED;
            s_press_time      = now_ms;
            s_hold_hint_sent  = false;
            s_super_hint_sent = false;
            s_second_click    = false;
            break;

        case ST_CLICK_PENDING:
            /* 第二击到来 -> 判定为双击，撤销待执行的单击 */
            s_second_click      = true;
            s_first_click_valid = false;
            s_state             = ST_PRESSED;
            s_press_time        = now_ms;
            s_hold_hint_sent    = false;
            s_super_hint_sent   = false;
            break;

        case ST_PRESSED:
        default:
            /* 重复的按下事件，忽略 */
            break;
        }
        return;
    }

    /* ---- 松开：此刻统一决策 ---- */
    if (s_state != ST_PRESSED) {
        return;
    }

    uint32_t held = now_ms - s_press_time;

    if (s_second_click) {
        /* 第二击的松开 -> 双击完成（长按/超长按在第二击时不适用） */
        s_state             = ST_IDLE;
        s_second_click      = false;
        s_first_click_valid = false;
        fire(GESTURE_DOUBLE_CLICK);
        return;
    }

    if (held >= s_cfg.super_long_press_ms) {
        /* 超长按：与长按互斥，只执行这一个动作 */
        s_state             = ST_IDLE;
        s_first_click_valid = false;
        fire(GESTURE_SUPER_LONG);
        return;
    }

    if (held >= s_cfg.long_press_ms) {
        /* 长按 */
        s_state             = ST_IDLE;
        s_first_click_valid = false;
        fire(GESTURE_LONG_PRESS);
        return;
    }

    if (held > s_cfg.click_max_ms) {
        /* 按住过久但未达长按阈值：本次按压无效 */
        s_state = ST_IDLE;
        if (s_first_click_valid) {
            s_first_click_valid = false;
            fire(GESTURE_SINGLE_CLICK);
        }
        s_second_click = false;
        return;
    }

    /* 短按 -> 进入单击候选窗口 */
    s_state             = ST_CLICK_PENDING;
    s_first_click_valid = true;
    s_click_deadline    = now_ms + s_cfg.double_click_gap_ms;
}

void button_fsm_tick(uint32_t now_ms)
{
    switch (s_state) {
    case ST_PRESSED: {
        uint32_t held = now_ms - s_press_time;

        /* 按住期间只发 UI 提示，不执行动作 —— 保证长按与超长按互斥 */
        if (!s_super_hint_sent && held >= s_cfg.super_long_press_ms) {
            s_super_hint_sent = true;
            fire(GESTURE_HOLD_SUPER);
        } else if (!s_hold_hint_sent && held >= s_cfg.long_press_ms) {
            s_hold_hint_sent = true;
            fire(GESTURE_HOLD_START);
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
