/**
 * @file ui_font.h
 * @brief BiliMouse UI 点阵字库 (自动生成，请勿手改)
 *
 * 由 tools/gen_font.py 生成：
 *   - 中文 16x16 (微软雅黑 Bold)
 *   - ASCII 8x16 (Consolas)
 *
 * 存储格式: 每个字符 {bpr, row[16]}，row 为 MSB-first 位图。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FONT_CJK_W       16
#define UI_FONT_CJK_H       16
#define UI_FONT_ASCII_W     8
#define UI_FONT_ASCII_H     16

#define UI_FONT_CJK_COUNT   114
#define UI_FONT_ASCII_COUNT 95
#define UI_FONT_ASCII_FIRST 0x20
#define UI_FONT_ASCII_LAST  0x7E

/** 查表：返回字符位图指针 (bpr + rows)，未找到返回 NULL */
const uint8_t *ui_font_get_cjk(uint16_t codepoint);
const uint8_t *ui_font_get_ascii(uint8_t ch);

#ifdef __cplusplus
}
#endif
