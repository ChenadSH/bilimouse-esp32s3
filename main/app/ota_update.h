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
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 升级流程阶段（两段式：先「检查」，检测到新版本才允许「升级」） */
typedef enum {
    OTA_PHASE_IDLE = 0,     /* 未检查 */
    OTA_PHASE_WIFI,         /* 正在连 WiFi */
    OTA_PHASE_MANIFEST,     /* 正在获取版本清单 */
    OTA_PHASE_READY,        /* 发现新版本，等待用户点「立即升级」 */
    OTA_PHASE_LATEST,       /* 已是最新 */
    OTA_PHASE_DOWNLOAD,     /* 下载写入中 */
    OTA_PHASE_FAIL,         /* 某一步失败 */
} ota_phase_t;

/** 检查/升级状态快照（升级任务后台写，UI 读）。
 *  屏幕按「WiFi / 清单 / 版本」三行分步显示，每一步成功失败一目了然，
 *  不再是按了按钮就黑盒卡住。 */
typedef struct {
    ota_phase_t phase;
    char wifi_info[36];     /* 第1步结果：正在连 <SSID> / 已连 <SSID> / 失败原因 */
    char list_info[36];     /* 第2步结果：获取中… / 已获取 / 获取失败 */
    char cmp_info[48];      /* 第3步结果：已是最新(服务器版本) / 发现新版本 */
    char cur_version[24];   /* 当前固件版本 */
    char new_version[24];   /* 服务器新版本（仅在 READY 时有效） */
    int  total_kb;          /* OTA 包总大小（KB），未知为 0 */
    int  done_kb;           /* 已下载（KB） */
    int  percent;           /* 0..100，下载中有效，未知为 -1 */
} ota_state_t;

/** 初始化 WiFi 子系统（不连接，连接在首次需要时发生）。幂等。 */
esp_err_t ota_update_init(void);

/** 是否已拿到 IP */
bool ota_update_wifi_connected(void);

/** 阻塞确保 WiFi 在线（未连则按凭据序列尝试）。电台等模块共用。 */
esp_err_t ota_update_wifi_online(void);

/** 读取检查/升级状态快照 */
void ota_update_get_state(ota_state_t *out);

/**
 * 第 1 步：开始检查更新（独立任务：连 WiFi -> 拉清单 -> 比版本）。
 * 只查不升 —— 检测到新版本后停在 OTA_PHASE_READY，等用户点「立即升级」。
 */
void ota_update_check(void);

/** 第 2 步：下载并安装已检测到的新版本（须先 ota_update_check 到 READY） */
void ota_update_download(void);

#ifdef __cplusplus
}
#endif
