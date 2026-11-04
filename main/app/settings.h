/**
 * @file settings.h
 * @brief 配置持久化（NVS）
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 体感灵敏度档位 */
typedef enum {
    SENS_LOW = 0,
    SENS_MEDIUM = 1,
    SENS_HIGH = 2,
} sensitivity_t;

/** 完整配置 */
typedef struct {
    int32_t  next_dx;        /* 校准：到"下一集"按钮的相对 X 位移 */
    int32_t  next_dy;        /* 校准：到"下一集"按钮的相对 Y 位移 */
    int32_t  prev_dx;        /* 校准：到"上一集"按钮的相对 X 位移 */
    int32_t  prev_dy;        /* 校准：到"上一集"按钮的相对 Y 位移 */
    bool     next_calibrated;
    bool     prev_calibrated;
    sensitivity_t sensitivity;
    uint8_t  backlight;      /* 背光亮度 0~100 */
    uint16_t debounce_ms;
    uint16_t long_press_ms;
    uint16_t super_long_press_ms;
    uint16_t ultra_long_press_ms;

    /* 触摸坐标映射：screen = k[0..2]*raw + k[3..5] */
    bool     touch_calibrated;
    float    touch_k[6];
} settings_t;

/** 加载配置（无存档时填默认值） */
esp_err_t settings_load(settings_t *cfg);

/** 保存配置 */
esp_err_t settings_save(const settings_t *cfg);

/** 恢复默认配置并保存 */
void settings_reset_default(settings_t *cfg);

/** 全局配置（settings_load 之后有效），供各模块读取 */
const settings_t *settings_get(void);

/** 全局配置的可写指针，改完需自行调用 settings_save */
settings_t *settings_mut(void);

#ifdef __cplusplus
}
#endif
