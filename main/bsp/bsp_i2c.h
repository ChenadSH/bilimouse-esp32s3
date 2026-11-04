/**
 * @file bsp_i2c.h
 * @brief I2C 总线统一管理层
 *
 * 开发板上有 5 个 I2C 设备共用同一条总线：
 *   PCA9557 (0x19) / FT6336 (0x38) / QMI8658 (0x6A) / ES8311 / ES7210
 *
 * ESP-IDF v6 的 i2c_new_master_bus() 对同一端口只允许创建一次总线，
 * 第二次调用返回 ESP_ERR_INVALID_STATE 且**不会填充输出句柄**。
 * 因此必须由本模块统一持有总线句柄，各驱动只负责挂自己的设备。
 */
#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 获取共享的 I2C 总线句柄（首次调用时创建）
 * @return ESP_OK 成功；句柄由本模块持有，不要手动释放
 */
esp_err_t bsp_i2c_bus_get(i2c_master_bus_handle_t *bus);

/**
 * 在共享总线上挂载一个设备
 * @param addr 设备 7 位地址
 * @param out  输出的设备句柄
 */
esp_err_t bsp_i2c_add_device(uint16_t addr, i2c_master_dev_handle_t *out);

/** 探测总线上是否存在某地址的设备 */
bool bsp_i2c_probe(uint16_t addr);

#ifdef __cplusplus
}
#endif
