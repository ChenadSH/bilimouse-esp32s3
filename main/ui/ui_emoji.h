/**
 * @file ui_emoji.h
 * @brief 首页 icon 的彩色 emoji 位图（自动生成，请勿手改）
 *
 * 由 tools/gen_emoji.py 生成：Segoe UI Emoji 彩色字形 -> RGB565，
 * 已按磁贴底色预合成，固件端直接贴图即可（无需 alpha 混合）。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_EMOJI_SZ 72

/** 首页 icon 顺序（= 首页轮播顺序） */
typedef enum {
    UI_EMOJI_BILI = 0,   /* 📺 B站视频控制 */
    UI_EMOJI_MOUSE,      /* 🖱 鼠标模拟 */
    UI_EMOJI_IR,         /* 📡 红外模拟 */
    UI_EMOJI_RADIO,      /* 🎵 网络电台 */
    UI_EMOJI_OTA,        /* 🔄 在线升级 */
    UI_EMOJI_CAL,        /* 🎯 触摸校准 */
    UI_EMOJI_COUNT
} ui_emoji_t;

extern const uint16_t ui_emoji_bili[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_mouse[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_ir[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_radio[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_ota[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_cal[UI_EMOJI_SZ * UI_EMOJI_SZ];

/** 每个 icon 的磁贴底色 / 描边色（RGB565） */
extern const uint16_t ui_emoji_tile_bg[UI_EMOJI_COUNT];
extern const uint16_t ui_emoji_tile_border[UI_EMOJI_COUNT];

#ifdef __cplusplus
}
#endif
