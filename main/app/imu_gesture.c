#include "imu_gesture.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "imu_gesture";

/* 灵敏度档位对应的阈值 */
typedef struct {
    float shake_gyro_dps;   /* 甩腕角速度阈值 */
    float tilt_angle_deg;   /* 倾斜角阈值 */
    float flip_z_thresh;    /* 翻转判定：|az| 需接近 1g */
    uint32_t tilt_hold_ms;  /* 倾斜需保持的时长 */
} thresholds_t;

static thresholds_t s_th;

/* 状态 */
static float s_prev_gyro_y = 0;
static float s_prev_accel_z = 0;
static bool  s_have_prev = false;
static uint32_t s_tilt_start_ms = 0;
static int    s_tilt_dir = 0;      /* -1 左倾, +1 右倾, 0 无 */
static uint32_t s_last_gesture_ms = 0;

static const thresholds_t s_th_table[] = {
    /* 低灵敏度：阈值高，不易触发 */
    { 260.0f, 55.0f, 0.80f, 1200 },
    /* 中灵敏度 */
    { 200.0f, 45.0f, 0.70f, 2000 },
    /* 高灵敏度：阈值低，易触发 */
    { 150.0f, 35.0f, 0.60f, 1500 },
};

void imu_gesture_init(sensitivity_t sens)
{
    int idx = (int)sens;
    if (idx < 0) idx = 0;
    if (idx > 2) idx = 2;
    s_th = s_th_table[idx];

    s_have_prev = false;
    s_tilt_start_ms = 0;
    s_tilt_dir = 0;
    s_last_gesture_ms = 0;

    ESP_LOGI(TAG, "sensitivity=%d shake=%.0fdps tilt=%.0fdeg", idx,
             s_th.shake_gyro_dps, s_th.tilt_angle_deg);
}

imu_gesture_t imu_gesture_process(const bsp_imu_data_t *data)
{
    if (!data) {
        return IMU_GESTURE_NONE;
    }

    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;

    /* 手势之间至少间隔 800ms，避免连续触发 */
    if ((uint32_t)(now - s_last_gesture_ms) < 800) {
        s_prev_gyro_y = data->gyro_dps[1];
        s_prev_accel_z = data->accel_g[2];
        s_have_prev = true;
        return IMU_GESTURE_NONE;
    }

    if (!s_have_prev) {
        s_prev_gyro_y = data->gyro_dps[1];
        s_prev_accel_z = data->accel_g[2];
        s_have_prev = true;
        return IMU_GESTURE_NONE;
    }

    /* ---- 甩腕检测：陀螺仪 Y 轴角速度绝对值超阈值 ---- */
    float gyro_y = (float)data->gyro_dps[1];
    float abs_gyro_y = gyro_y < 0 ? -gyro_y : gyro_y;
    float prev_abs_gyro_y = s_prev_gyro_y < 0 ? -s_prev_gyro_y : s_prev_gyro_y;

    if (abs_gyro_y > s_th.shake_gyro_dps) {
        /* 需要连续两帧都超阈值才算一次完整甩动，避免单帧噪声 */
        if (prev_abs_gyro_y > s_th.shake_gyro_dps * 0.7f) {
            s_last_gesture_ms = now;
            s_prev_gyro_y = data->gyro_dps[1];
            s_prev_accel_z = data->accel_g[2];
            ESP_LOGI(TAG, "shake detected (gyro_y=%.1f)", data->gyro_dps[1]);
            return IMU_GESTURE_SHAKE;
        }
    }

    /* ---- 翻转检测：加速度计 Z 轴符号翻转且接近 ±1g ---- */
    float accel_z = (float)data->accel_g[2];
    float abs_accel_z = accel_z < 0 ? -accel_z : accel_z;
    if (abs_accel_z > s_th.flip_z_thresh) {
        if (s_prev_accel_z * accel_z < 0) {
            s_last_gesture_ms = now;
            s_prev_gyro_y = data->gyro_dps[1];
            s_prev_accel_z = data->accel_g[2];
            ESP_LOGI(TAG, "flip detected (az=%.2f)", data->accel_g[2]);
            return IMU_GESTURE_FLIP;
        }
    }

    /* ---- 倾斜检测：重力在 X 轴的分量 -> 倾角 ---- */
    float ax = (float)data->accel_g[0];
    float az = (float)data->accel_g[2];
    float denom = sqrtf(ax * ax + az * az);
    if (denom > 0.1f) {
        float angle = atan2f(fabsf(ax), az) * 180.0f / 3.14159265358979f;
        int dir = (ax > 0) ? 1 : -1;

        if (angle > s_th.tilt_angle_deg) {
            if (s_tilt_dir != dir) {
                s_tilt_dir = dir;
                s_tilt_start_ms = now;
            } else if (now - s_tilt_start_ms >= s_th.tilt_hold_ms) {
                s_last_gesture_ms = now;
                s_tilt_start_ms = 0;
                s_prev_gyro_y = data->gyro_dps[1];
                s_prev_accel_z = data->accel_g[2];
                ESP_LOGI(TAG, "tilt detected (angle=%.1f dir=%d)", angle, dir);
                return (dir > 0) ? IMU_GESTURE_TILT_RIGHT : IMU_GESTURE_TILT_LEFT;
            }
        } else {
            s_tilt_dir = 0;
            s_tilt_start_ms = 0;
        }
    }

    s_prev_gyro_y = data->gyro_dps[1];
    s_prev_accel_z = data->accel_g[2];
    return IMU_GESTURE_NONE;
}
