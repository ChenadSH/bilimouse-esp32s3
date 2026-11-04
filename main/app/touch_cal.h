/**
 * @file touch_cal.h
 * @brief 触摸坐标映射与三点校准向导
 *
 * 为什么需要校准：
 *   FT6336 输出的坐标单位、轴向、方向都取决于面板是怎么贴在屏幕上的
 *   （本项目面板是 240x320 竖装，屏幕却是 320x240 横屏）。
 *   写死一个"旋转 90°+翻转"的公式，只要换一批板子/换一个装配方向就全错，
 *   而且错了以后现象是"能读到触摸，但点哪儿都不准"——极难排查。
 *
 * 做法：让用户点 3 个已知位置的点（左上、右上、左下），
 *   由这 3 组「屏幕坐标 <-> 面板原始坐标」直接解出仿射矩阵：
 *       screen_x = k0*raw_x + k1*raw_y + k2
 *       screen_y = k3*raw_x + k4*raw_y + k5
 *   旋转、镜像、X/Y 交换、缩放全部被这一个矩阵吸收，
 *   不依赖任何硬件假设。结果存 NVS，只需做一次。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 向导阶段 */
typedef enum {
    TOUCH_CAL_IDLE = 0,     /* 未运行 */
    TOUCH_CAL_TAP1,         /* 等待点击左上角 */
    TOUCH_CAL_TAP2,         /* 等待点击右上角 */
    TOUCH_CAL_TAP3,         /* 等待点击左下角 */
    TOUCH_CAL_VERIFY,       /* 已生成映射，验证触点是否跟手 */
} touch_cal_stage_t;

/** 校准完成回调（ok=false 表示超时/失败退出） */
typedef void (*touch_cal_done_cb_t)(bool ok);

/** 从配置载入映射（必须在 settings_load 之后调用） */
void touch_cal_init(touch_cal_done_cb_t cb);

/** 是否正在校准（此时触摸由向导独占，不产生鼠标动作） */
bool touch_cal_is_active(void);

/** 当前阶段 */
touch_cal_stage_t touch_cal_stage(void);

/** 启动向导（重新校准） */
void touch_cal_start(void);

/**
 * 启动向导，并告知"启动这一刻手指是否正按在屏上"
 *
 * 若为 true，则这一次按下/抬起不算采样点 —— 否则用户"点一下屏幕把向导叫出来"
 * 的动作会被当成第 1 个点记录，必然错位。
 */
void touch_cal_start_ex(bool pressed_now);

/** 取消向导 */
void touch_cal_cancel(void);

/**
 * 喂入一次触摸采样（每帧调用，需包含松开状态以便识别"按下"边沿）
 * @return true 表示本次触摸被向导消费，调用方不应再做鼠标/按钮处理
 */
bool touch_cal_feed(bool pressed, uint16_t raw_x, uint16_t raw_y);

/** 原始坐标 -> 屏幕坐标（未校准时走默认映射并做范围自适应） */
void touch_cal_map(uint16_t raw_x, uint16_t raw_y, uint16_t *sx, uint16_t *sy);

/** 当前阶段的目标点（屏幕坐标） */
void touch_cal_target(touch_cal_stage_t stage, int *x, int *y);

/** 最近一次触点（屏幕坐标），用于验证阶段画十字 */
void touch_cal_last_point(int *x, int *y, bool *valid);

/** 是否已校准过 */
bool touch_cal_is_calibrated(void);

#ifdef __cplusplus
}
#endif
