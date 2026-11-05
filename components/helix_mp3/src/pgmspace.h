/**
 * @file pgmspace.h
 * @brief ESP-IDF 兼容层：helix 源码来自 Arduino 生态，用 avr/pgmspace 风格宏。
 *        ESP-IDF 的 flash 常量不需要特殊属性，这里全部映射成普通访问。
 *        （本文件放组件 include 目录，同名 <> 包含优先命中本文件）
 */
#pragma once

#ifndef PROGMEM
#define PROGMEM
#endif

#define pgm_read_byte(p)   (*(const uint8_t *)(p))
#define pgm_read_word(p)   (*(const uint16_t *)(p))
#define pgm_read_dword(p)  (*(const uint32_t *)(p))
