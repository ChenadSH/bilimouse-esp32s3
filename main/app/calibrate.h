/**
 * @file calibrate.h
 * @brief 校准流程状态机
 *
 * 目标：让「下一集」按钮能被一次按键命中。
 *
 * 原理：BLE 鼠标只能发相对位移，设备不知道主机光标在哪。
 *      本模块用「增量路径」方案：
 *        1. 用户把主机光标停在 B 站「下一集」按钮上
 *        2. 设备进入校准，记录当前虚拟位置为起点
 *        3. 用户用设备（触摸滑动 / 物理鼠标）把光标移到按钮上
 *        4. 设备记录这段位移差 = 「下一集」的点击路径
 *      之后每次切集，设备重放该位移差并左键点击。
 *
 * 交互（全部在设备 2 寸屏上完成，躺着也能操作）：
 *   校准模式下屏幕显示两个按钮：[下一集] [上一集]
 *   点「下一集」-> 记录起点，屏幕提示"用鼠标移到下一集按钮后点此确认"
 *   再点一次确认 -> 计算位移差并存入 NVS
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pointer.h"   /* cal_target_t */

#ifdef __cplusplus
extern "C" {
#endif

/** 校准状态 */
typedef enum {
    CAL_STATE_IDLE = 0,      /* 未校准 */
    CAL_STATE_ARMED,         /* 已记录起点，等待用户移动光标并确认 */
    CAL_STATE_DONE,          /* 刚完成校准 */
} cal_state_t;

/** 初始化 */
void calibrate_init(void);

/** 进入校准模式（由 UI 或按键触发） */
void calibrate_enter(void);

/** 退出校准模式 */
void calibrate_exit(void);

/** 当前是否处于校准模式 */
bool calibrate_is_active(void);

/**
 * 处理屏幕上的校准按钮点击
 * @param target 要校准的目标
 */
void calibrate_select_target(cal_target_t target);

/** 当前校准状态（供 UI 显示） */
cal_state_t calibrate_state(void);

/** 当前正在校准的目标 */
int calibrate_current_target(void);

#ifdef __cplusplus
}
#endif
