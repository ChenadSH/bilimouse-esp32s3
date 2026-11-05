/**
 * @file bsp_audio.h
 * @brief 音频输出 BSP：ES8311 DAC + I2S TX + NS4150B 功放
 *
 * 硬件链路（立创实战派 LCKFB-SZPI-ESP32-S3-VA）：
 *   I2S: BCLK=GPIO14, WS=GPIO13, DOUT=GPIO45（MCLK 引脚与 LCD_RST 共用 GPIO38，
 *        因此 ES8311 配置为从 SCLK 引脚内部倍频取 MCLK，不接 MCLK 线）
 *   I2C: ES8311 地址 0x18，与触摸/IMU 共用 GPIO1/2 总线（bsp_i2c 统一管理）
 *   功放: NS4150B，使能脚 = PCA9557 IO1（高电平开启）
 *
 * 使用顺序：
 *   bsp_audio_init() 一次；
 *   bsp_audio_start(rate) -> bsp_audio_write(...) 循环 -> bsp_audio_stop()。
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化：I2S 通道创建 + ES8311 复位/配置。幂等。 */
esp_err_t bsp_audio_init(void);

/**
 * 开始播放：配置采样率（会同步重配 ES8311 时钟）+ 开功放。
 * @param sample_rate 解码器输出的采样率（如 44100/48000）
 */
esp_err_t bsp_audio_start(uint32_t sample_rate);

/** 写 PCM（16bit 交错双声道）。缓冲满时阻塞。 */
esp_err_t bsp_audio_write(const int16_t *pcm, size_t samples);

/** 停止播放：静音 + 关功放（保留 I2S 通道，避免频繁重建）。 */
esp_err_t bsp_audio_stop(void);

/** 音量 0~100（存 NVS，重启保持） */
esp_err_t bsp_audio_volume_set(int vol);
int bsp_audio_volume_get(void);

#ifdef __cplusplus
}
#endif
