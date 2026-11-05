/**
 * @file ir_backup.h
 * @brief 红外码库的导出/导入/备份（串口控制台命令）
 *
 * 为什么走串口而不是屏幕/蓝牙：一份完整码库（10 槽 × 最多 96 符号）
 * 有几 KB，屏幕装不下，蓝牙侧也不适合做文件通道；而设备本来就通过
 * USB 串口输出日志，`idf.py monitor` 里就能交互，零硬件成本。
 *
 * 命令（注册到 esp_console，见 cli.c）：
 *   ir list                 列出各槽状态
 *   ir export all           全部导出（每槽一行，可直接另存为备份文件）
 *   ir export <slot>        导出单槽
 *   ir import <slot> <b64>  导入单槽（写 NVS + irbank 双副本）
 *   ir clear                清空（危险操作，需确认参数）
 *
 * 导出行格式（一槽一行，方便整行复制/粘贴回 `ir import`）：
 *   BMIR1 slot=<n> syms=<count> b64=<base64(2字节count + count*4字节原始符号)>
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化：向全局 esp_console 注册 ir_* 命令（幂等） */
esp_err_t ir_backup_register(void);

#ifdef __cplusplus
}
#endif
