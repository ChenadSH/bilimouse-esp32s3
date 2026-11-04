/**
 * @file button_fsm.h
 * @brief 单键多义手势状态机
 *
 * 一个按键表达四种意图，靠时序区分：
 *   单击   -> 下一集
 *   双击   -> 上一集
 *   长按   -> 播放/暂停
 *   超长按 -> 鼠标模式开关
 *
 * 判定规则（实现必须严格遵循）：
 *   按下 -> 计时
 *     |  <250ms 松开        -> 单击候选，等 300ms 看有无第二击
 *     |     + 有第二击       -> 双击（取消单击）
 *     |     + 超时无第二击   -> 执行单击
 *     |  600ms 到达          -> 触发长按（只触发一次）
 *     |     + 继续按住到1500ms -> 触发超长按（覆盖长按）
 *
 * 关键约束：一次按下最多触发一个动作，不得连发。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 识别出的手势 */
typedef enum {
    GESTURE_NONE = 0,
    GESTURE_SINGLE_CLICK,   /* 单击 */
    GESTURE_DOUBLE_CLICK,   /* 双击 */
    GESTURE_LONG_PRESS,     /* 长按 */
    GESTURE_SUPER_LONG,     /* 超长按 */
} gesture_t;

/** 事件回调（在手势确定时触发） */
typedef void (*gesture_cb_t)(gesture_t gesture, void *ctx);

/** 手势判定参数 */
typedef struct {
    uint16_t click_max_ms;        /* 超过此时长不算单击 */
    uint16_t double_click_gap_ms; /* 双击判定窗口 */
    uint16_t long_press_ms;       /* 长按阈值 */
    uint16_t super_long_press_ms; /* 超长按阈值 */
} fsm_config_t;

/**
 * 初始化状态机
 */
void button_fsm_init(const fsm_config_t *cfg, gesture_cb_t cb, void *ctx);

/**
 * 处理按键状态变化（由按键任务调用）
 * @param pressed true=按下, false=松开
 * @param now_ms  当前时间戳（毫秒）
 */
void button_fsm_update(bool pressed, uint32_t now_ms);

/**
 * 推进超时判定（需要在无按键事件时定期调用以触发单击/双击超时）
 * @param now_ms 当前时间戳
 */
void button_fsm_tick(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
