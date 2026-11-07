/**
 * @file bsp_ir.h
 * @brief 红外收发（RMT 外设，38kHz 载波）
 *
 * 硬件接线（板载 GH1.25 扩展口，无需飞线）：
 *   发射：红外 LED（或带三极管的发射模块）信号脚 -> GPIO10
 *   接收：HX1838 / VS1838B 等 38kHz 一体化接收头 OUT -> GPIO11
 *         VCC 接 3V3，GND 接 GND
 *
 * 为什么用 RMT 而不是软件延时：
 *   NEC 的 560us / 1690us 脉宽容差只有 ±100us 左右，任务调度抖动
 *   就会把码打歪。RMT 由硬件产生波形，精度是 1 个 1MHz tick。
 *
 * 为什么发射用"载波调制 + 包络符号"：
 *   RMT 的 carrier 会在 level=1 的符号上叠加 38kHz 方波，所以只需要
 *   构造"亮 9ms / 灭 4.5ms"这种包络，不用手工拼 38kHz 的脉冲串。
 *   接收端打开 invert_in，把接收头的低电平有效翻转成同样的 level=1，
 *   学习到的波形可以原样回放（同一份 symbol 数组）。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "driver/rmt_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 扩展接口 GPIO（见 docs/HARDWARE.md「扩展接口 GPIO10、GPIO11」） */
#define BSP_IR_TX_GPIO      10
#define BSP_IR_RX_GPIO      11

/** 单帧最多缓存的 symbol 数（1 symbol = 一对「电平+时长」）。
   96 只够 NEC(67)；空调遥控器的整帧状态码普遍 100~180 个符号，
   96 会截断成永远发射无效的残码 —— 已配合 RMT DMA 扩到 192。 */
#define BSP_IR_MAX_SYMBOLS  192

/** 初始化 RMT 收发通道（未接硬件也能成功，只是收不到信号） */
esp_err_t bsp_ir_init(void);

/** 是否初始化成功 */
bool bsp_ir_ready(void);

/**
 * 发送标准 NEC 码（含 9ms+4.5ms 引导码、32 位地址/命令反码、560us 结束位）
 * @param addr  8 位地址
 * @param cmd   8 位命令
 */
esp_err_t bsp_ir_send_nec(uint8_t addr, uint8_t cmd);

/**
 * 原样回放一份 symbol 波形（学习到的码）
 * @param syms  symbol 数组
 * @param count 符号数
 */
esp_err_t bsp_ir_send_raw(const rmt_symbol_word_t *syms, size_t count);

/**
 * 等待并捕获一帧红外信号（阻塞）。
 *
 * @param out        输出缓冲
 * @param max_symbols 缓冲容量
 * @param timeout_ms 最长等待时间
 * @return 捕获到的符号数；0 表示超时或失败
 */
size_t bsp_ir_capture(rmt_symbol_word_t *out, size_t max_symbols, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
