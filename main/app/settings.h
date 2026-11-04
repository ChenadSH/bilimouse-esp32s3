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
} settings_t;

/** 加载配置（无存档时填默认值） */
esp_err_t settings_load(settings_t *cfg);

/** 保存配置 */
esp_err_t settings_save(const settings_t *cfg);

/** 恢复默认配置并保存 */
void settings_reset_default(settings_t *cfg);

#ifdef __cplusplus
}
#endif
