/**
 * @file bsp_board.h
 * @brief 立创·实战派 ESP32-S3 开发板 (LCKFB-SZPI-ESP32-S3-VA) 板级定义
 *
 * 引脚依据：立创官方文档中心 + 同板真机验证项目交叉确认
 *   https://wiki.lckfb.com/zh-hans/szpi-esp32s3/
 */
#pragma once

#include <stdint.h>
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== I2C 总线 ======================== */
/* PCA9557 / FT6336 / QMI8658 / ES8311 / ES7210 共用同一条 I2C 总线 */
#define BSP_I2C_PORT      I2C_NUM_0
#define BSP_I2C_SDA       GPIO_NUM_1
#define BSP_I2C_SCL       GPIO_NUM_2
#define BSP_I2C_FREQ_HZ   400000

/* 从设备地址 */
#define BSP_PCA9557_ADDR  0x19   /* IO 扩展芯片 */
#define BSP_FT6336_ADDR   0x38   /* 电容触摸控制器 */
#define BSP_QMI8658_ADDR  0x6A   /* 6D 姿态传感器 */

/* ======================== LCD (ST7789, SPI3) ======================== */
#define BSP_LCD_SPI_HOST  SPI3_HOST
#define BSP_LCD_PIN_SCLK  GPIO_NUM_41
#define BSP_LCD_PIN_MOSI  GPIO_NUM_40
#define BSP_LCD_PIN_DC    GPIO_NUM_39
#define BSP_LCD_PIN_RST   GPIO_NUM_38
#define BSP_LCD_PIN_BL    GPIO_NUM_42   /* 低电平点亮，LEDC 通道 0 PWM 调光 */

/* LCD 片选由 PCA9557 IO0 驱动（低电平有效），不可直接用 GPIO 控制 */
#define BSP_PCA9557_LCD_CS_BIT  0

/* 分辨率：横屏 320x240 */
#define BSP_LCD_HOR_RES   320
#define BSP_LCD_VER_RES   240

/* ======================== 按键 ======================== */
/* 板载用户按键 = BOOT 键复用（GPIO0），低电平按下 */
#define BSP_BUTTON_GPIO   GPIO_NUM_0
#define BSP_BUTTON_ACTIVE_LOW  1

/* ======================== 板载 LED ======================== */
#define BSP_LED_GPIO      GPIO_NUM_48

#ifdef __cplusplus
}
#endif
