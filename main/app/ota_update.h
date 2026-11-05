/**
 * @file ota_update.h
 * @brief WiFi 联网 + OTA 在线升级
 *
 * 升级数据流（详见 docs/OTA.md）：
 *
 *   设备                         升级服务器（任意静态 HTTP 服务）
 *    |  GET manifest.json           |
 *    |------------------------------->  { version, url, notes }
 *    |  版本比较：manifest > 当前 ?   |
 *    |  GET url（分块流式写入 ota_x） |
 *    |<-------------------------------|
 *    |  esp_ota_set_boot_partition   |
 *    |  5s 倒计时 -> esp_restart     |
 *
 * 安全网（三道）：
 *   1. esp_https_ota_finish 内置镜像校验（magic / 芯片型号 / 长度）；
 *   2. CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE：新固件第一次启动处于
 *      PENDING_VERIFY，app_main 启动成功后调用
 *      esp_ota_mark_app_valid_cancel_rollback() 确认；启动失败
 *      （崩溃/看门狗）则 bootloader 自动回滚到上一个可用槽；
 *   3. 升级只写备槽，出厂槽与 NVS/irbank 数据分区全程不被触碰。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 WiFi 子系统（不连接，连接在首次需要时发生）。幂等。 */
esp_err_t ota_update_init(void);

/** 是否已拿到 IP */
bool ota_update_wifi_connected(void);

/**
 * 手动升级入口（屏幕「升级」按钮）：独立任务里执行
 * 连 WiFi -> 查 manifest -> 有新版就下载安装 -> 倒计时重启。
 */
void ota_update_start_manual(void);

#ifdef __cplusplus
}
#endif
