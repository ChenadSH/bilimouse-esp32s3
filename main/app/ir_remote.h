/**
 * @file ir_remote.h
 * @brief 红外遥控学习库：学习 -> 双份持久化 -> 原样回放
 *
 * 不猜协议，不解析码值。把接收头收到的原始波形（RMT symbol 序列）
 * 原样存下来，回放时用同一个数组再发一次 —— 只要能收到，就一定能发出去，
 * 对空调、机顶盒、风扇这类非标准协议的遥控器同样有效。
 *
 * 存档：NVS + 独立 "irbank" flash 分区双副本（见 ir_store.h）。
 * 重启、断电、重新烧录固件都不会丢；只有 `idf.py erase-flash` 或者
 * 故意去改 partitions.csv 里 nvs / irbank 的 offset 才会丢。
 *
 * 槽位：10 个，用屏上的「切换」按钮选择当前槽；当前槽号也会记住。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_SLOT_COUNT 10

/** v1 旧格式的槽位数（仅迁移用，固定 4 个，别改） */
#define IR_SLOT_V1_COUNT 4

/** 初始化：初始化 RMT + 从 NVS 载入已学码 */
esp_err_t ir_remote_init(void);

/** RMT 是否可用 */
bool ir_remote_ready(void);

/**
 * 学习一帧：阻塞等待用户按遥控器，成功则写入当前槽并保存到 NVS。
 * @param timeout_ms 等待时长
 * @return ESP_OK 学到并保存；ESP_ERR_TIMEOUT 没收到信号
 */
esp_err_t ir_remote_learn(uint32_t timeout_ms);

/** 发射当前槽（未学习过则返回 ESP_ERR_NOT_FOUND） */
esp_err_t ir_remote_send(void);

/** 当前槽是否有码 */
bool ir_remote_slot_loaded(uint8_t slot);

/** 当前选中槽（0..IR_SLOT_COUNT-1） */
uint8_t ir_remote_active_slot(void);

/** 切到下一个槽（循环） */
void ir_remote_next_slot(void);

/** 已学习的槽数量 */
uint8_t ir_remote_loaded_count(void);

/** 清空全部学习记录 */
esp_err_t ir_remote_clear_all(void);

/**
 * 覆写指定槽（红外码导入用）。
 * @param count 1..192
 * @return ESP_OK 写入并保存到 NVS+irbank 双副本
 */
esp_err_t ir_remote_write_slot(uint8_t slot,
                               const void *syms, uint16_t count);

/** 当前码库的保存序号（导出/导入诊断用） */
uint32_t ir_remote_bank_seq(void);

/**
 * 读取指定槽的原始波形（红外码导出用）。
 * @param[out] count 实际符号数，空槽为 0
 * @return ESP_OK；ESP_ERR_NOT_FOUND 槽为空；ESP_ERR_INVALID_ARG 参数错误
 */
esp_err_t ir_remote_read_slot(uint8_t slot, void *syms, uint16_t *count);


#ifdef __cplusplus
}
#endif
