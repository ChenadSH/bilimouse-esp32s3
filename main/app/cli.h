/**
 * @file cli.h
 * @brief 串口命令行（esp_console REPL）
 *
 * 用途：
 *   - 红外码库备份：ir_export / ir_import（见 ir_backup.h）
 *   - WiFi / OTA 配置：wifi set ... / ota url ...（见 ota_update.c）
 *
 * 只占 UART0（就是日志口），`idf.py monitor` 里直接敲命令即可。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 启动 REPL（注册所有模块命令）。幂等。 */
esp_err_t cli_start(void);

#ifdef __cplusplus
}
#endif
