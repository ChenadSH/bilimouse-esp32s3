/**
 * @file radio.h
 * @brief WiFi 网络电台
 *
 * 台列表来源：仓库里的 radio/radio_stations.json（master 分支）。
 * 设备每次进入电台模式都实时拉取最新列表 —— 想增删台，只要在
 * GitHub 上改这个文件提交，设备不用重新烧录。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_MAX_STATIONS 16

/** URL 缓冲长度。
 *  CNR 的 HLS 地址带一长串会话参数（实测 213 字节），
 *  缓冲太小会把 URL 截断 -> 分段请求失败 -> 没声音。 */
#define RADIO_URL_MAX 256

typedef struct {
    char name[40];   /* 台名（截断到屏幕能显示的宽度） */
    char url[RADIO_URL_MAX];   /* MP3 直播流或 HLS m3u8 */
} radio_station_t;

/** 台列表拉取成功后才能拿到以下信息 */
int radio_station_count(void);
const radio_station_t *radio_station(int idx);
int radio_current_index(void);
bool radio_is_playing(void);

/** 当前状态文案（给 UI 卡片副标题），buf 至少 48 字节 */
void radio_status_text(char *buf, int len);

/**
 * 进入电台模式：连 WiFi -> 拉最新台列表 -> 显示第一个台。
 * 内部起独立任务，不阻塞调用方。重复调用安全。
 */
void radio_enter(void);

/** 选台（不自动播放；正在播放则切到新台继续播） */
void radio_next(void);
void radio_prev(void);

/** 播放/停止当前台 */
void radio_toggle(void);

/** 台列表点选：直接播放指定台（正在播放则无缝切换） */
void radio_select(int idx);

/** 强制停止播放（退出电台页面时用） */
void radio_stop(void);

/** 电台音量 0~100（存 NVS） */
void radio_volume(int vol);

/** 注册串口命令：radio list|play <n>|stop|vol <0-100>|update */
void radio_register_cli(void);

#ifdef __cplusplus
}
#endif
