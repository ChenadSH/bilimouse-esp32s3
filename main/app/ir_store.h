/**
 * @file ir_store.h
 * @brief 红外码库的「保险柜」：独立 flash 分区 + A/B 双扇区冗余
 *
 * 为什么不只写 NVS：
 *   1. NVS 是通用键值区，程序里任何一次 nvs_flash_erase()（例如
 *      NO_FREE_PAGES / NEW_VERSION_FOUND 的兜底处理）都会把它一起清掉；
 *   2. 换分区表、换固件版本时，NVS 版本号不匹配也会被整体擦除。
 *   学一次遥控器要按着遥控器等好几秒，丢了最恼人。
 *
 *   所以在 NVS 之外再开一个**只放红外码**的裸分区（label = "irbank"），
 *   用两个 4KB 扇区轮换写：
 *
 *     seq 小的 = 旧副本    seq 大的 = 新副本
 *     写新数据时先擦「旧」那一页，再写，写完回读校验；
 *     万一写一半掉电，旧的完好那一页仍在，加载时取 seq 更大且 CRC 对的。
 *
 *   两边都在，任意一边被清掉都能从另一边自动补回来。
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单条记录上限（不得超过 4KB 扇区 - 16 字节头 = 4080）。
 *  10 槽 × 388B + 12B 头 = 3892B，贴着上限放，改槽数时注意同步扩分区。 */
#define IR_STORE_MAX_REC  4080

/** 初始化：定位 irbank 分区。分区不存在时返回错误，但不算致命 */
esp_err_t ir_store_init(void);

/** 分区是否可用（分区表里有没有 irbank） */
bool ir_store_available(void);

/**
 * 读取最新一份有效记录。
 * @param buf      输出缓冲
 * @param cap      缓冲容量
 * @param out_len  实际长度（可 NULL）
 * @param out_seq  该记录的序号（可 NULL）
 * @return ESP_OK 读到且 CRC 校验通过；ESP_ERR_NOT_FOUND 没有有效记录
 */
esp_err_t ir_store_load(void *buf, size_t cap, size_t *out_len, uint32_t *out_seq);

/**
 * 写入一份新记录。
 * 内部流程：擦旧页 -> 写 -> 回读校验。任何一步失败都不会破坏旧副本。
 * @param seq 记录序号，由调用方维护（NVS 副本里存的是同一个序号，
 *            这样两份永远能比较出谁更新，也不会每次开机都互相"修复"一遍）
 */
esp_err_t ir_store_save(const void *buf, size_t len, uint32_t seq);

#ifdef __cplusplus
}
#endif
