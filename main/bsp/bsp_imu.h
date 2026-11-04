/**
 * @file bsp_imu.h
 * @brief QMI8658 六轴姿态传感器驱动（I2C）
 *
 * 用于体感手势：
 *   - 甩腕（陀螺仪角速度峰值）-> 快进
 *   - 翻转（加速度计 Z 轴符号变化）-> 播放/暂停
 *   - 倾斜保持 -> 音量调节
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float accel_g[3];   /* 加速度 (g) */
    float gyro_dps[3];  /* 角速度 (度/秒) */
    float temperature_c;
} bsp_imu_data_t;

/** 初始化 QMI8658：软复位 -> 配置量程/ODR -> 使能 */
esp_err_t bsp_imu_init(void);

/** 读取一帧 IMU 数据 */
esp_err_t bsp_imu_read(bsp_imu_data_t *data);

/** 读取 I2C 通信是否正常（探测芯片存在） */
bool bsp_imu_probe(void);

#ifdef __cplusplus
}
#endif
