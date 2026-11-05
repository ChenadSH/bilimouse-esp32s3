/**
 * @file ir_remote.h
 * @brief 红外遥控学习库：学习 -> 存 NVS -> 原样回放
 *
 * 不猜协议，不解析码值。把接收头收到的原始波形（RMT symbol 序列）
 * 原样存下来，回放时用同一个数组再发一次 —— 只要能收到，就一定能发出去，
 * 对空调、机顶盒、风扇这类非标准协议的遥控器同样有效。
 *
 * 槽位：4 个，用屏上的「切换」按钮选择当前槽。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IR_SLOT_COUNT 4

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

#ifdef __cplusplus
}
#endif
