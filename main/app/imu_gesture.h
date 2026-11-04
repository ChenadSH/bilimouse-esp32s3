/**
 * @file imu_gesture.h
 * @brief IMU 体感手势识别
 *
 * 手势定义：
 *   甩腕    -> 快进 10 秒（陀螺仪 Y 轴角速度峰值超阈值）
 *   翻转    -> 播放/暂停（加速度计 Z 轴符号翻转，接近 ±1g）
 *   倾斜保持 -> 音量增减（姿态角偏离水平超过阈值并保持）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bsp_imu.h"
#include "settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 识别出的体感手势 */
typedef enum {
    IMU_GESTURE_NONE = 0,
    IMU_GESTURE_SHAKE,     /* 甩腕 */
    IMU_GESTURE_FLIP,      /* 翻转 */
    IMU_GESTURE_TILT_LEFT, /* 左倾 */
    IMU_GESTURE_TILT_RIGHT,/* 右倾 */
} imu_gesture_t;

/** 初始化手势识别（根据灵敏度档位设置阈值） */
void imu_gesture_init(sensitivity_t sens);

/**
 * 输入一帧 IMU 数据，返回识别出的手势
 * @return IMU_GESTURE_NONE 表示本帧无手势
 */
imu_gesture_t imu_gesture_process(const bsp_imu_data_t *data);

#ifdef __cplusplus
}
#endif
