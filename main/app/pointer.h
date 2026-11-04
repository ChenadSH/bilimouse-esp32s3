/**
 * @file pointer.h
 * @brief 虚拟光标模块 —— 解决 BLE 鼠标相对位移的定位难题
 *
 * 背景问题：
 *   BLE HID 鼠标只能发送「相对位移」，设备无法读取主机的真实光标位置。
 *   而 B 站网页端的「下一集」按钮没有官方键盘快捷键，必须靠鼠标点击。
 *
 * 解法：
 *   设备自己维护一个「虚拟光标」——记录自校准以来累计发送的位移。
 *   校准流程：
 *     1. 用户把主机光标停在 B 站「下一集」按钮上
 *     2. 设备记录当前累计位移为「起点」
 *     3. 用户用设备（触摸滑动 / 体感）把光标移到按钮上，设备累计位移
 *     4. 设备把这段位移差存下来 = 「下一集」的点击路径
 *   之后每次切集：从当前虚拟位置重放该位移差，再发一次左键点击。
 *
 * 前提：校准后主机光标不应被其它鼠标大幅移动。这是本方案的唯一约束，
 *      对「躺在床上用一个设备操作」的典型场景完全成立。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 校准目标 */
typedef enum {
    CAL_TARGET_NEXT = 0,   /* 下一集 */
    CAL_TARGET_PREV,       /* 上一集 */
    CAL_TARGET_MAX
} cal_target_t;

/** 初始化虚拟光标（累计量归零） */
void pointer_init(void);

/**
 * 累加一次鼠标移动（所有鼠标位移都应经过这里）
 * @param dx X 位移
 * @param dy Y 位移
 */
void pointer_track(int32_t dx, int32_t dy);

/** 当前虚拟光标位置 */
void pointer_get_position(int32_t *x, int32_t *y);

/**
 * 开始校准：把当前虚拟位置记为起点
 * @return ESP_OK
 */
esp_err_t pointer_calibration_begin(cal_target_t target);

/**
 * 结束校准：计算起点到当前位置的位移差并存入 NVS
 * @return ESP_OK
 */
esp_err_t pointer_calibration_end(cal_target_t target);

/** 校准是否正在进行 */
bool pointer_is_calibrating(cal_target_t target);

/**
 * 移动到指定目标并点击（用于「下一集」「上一集」）
 * @param dx 目标相对位移（存于 NVS 的校准值）
 * @param dy 目标相对位移
 * @return ESP_OK 表示已执行
 */
esp_err_t pointer_move_and_click(int32_t dx, int32_t dy);

#ifdef __cplusplus
}
#endif
