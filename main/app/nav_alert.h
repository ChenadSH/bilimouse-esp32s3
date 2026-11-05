/**
 * @file nav_alert.h
 * @brief 导航提醒：手机快捷指令把导航提示推到设备屏幕 + 喇叭提示音
 *
 * 背景：真正的 CarPlay 投屏需要 Apple MFi 认证芯片与专用协议栈，
 * ESP32-S3 做不到。本模块是同类体验的替代方案：
 *
 *   iPhone 快捷指令（自动化：开始导航/每段路口提示）
 *     -> 「获取 URL 内容」或 GET:
 *        http://<设备IP>:8080/nav?text=300米后右转
 *     -> 设备屏幕显示 + 「叮咚」提示音
 *
 * 语音播报由手机端快捷指令的「朗读文本」完成（手机在车里本来就连着
 * 车机蓝牙），设备负责屏幕提醒与提示音。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单条导航提示的最大字节数（UTF-8 中文 3 字节/字，约 30 字） */
#define NAV_TEXT_MAX 96

/** 启动 HTTP 服务器（端口 8080）。幂等；失败不影响其它功能。 */
esp_err_t nav_alert_init(void);

/** 最近一条导航提示是否有效 */
bool nav_alert_has_message(void);

/** 当前 STA IP（未联网返回 "未连接"），导航页展示推送地址用 */
void nav_alert_get_ip(char *buf, int len);

/**
 * 取最近一条提示。
 * @param out_ts 输出收到时刻（秒级时间戳，可为 NULL）
 */
void nav_alert_get(char *buf, int len, int64_t *out_ts);

/** 手动注入一条提示（串口测试用），走同一条显示+提示音路径 */
void nav_alert_push(const char *text);

#ifdef __cplusplus
}
#endif
