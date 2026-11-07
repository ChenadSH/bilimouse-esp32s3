/**
 * @file hid_device.c
 * @brief BiliMouse BLE HID 应用层实现
 *
 * 基于 ESP-IDF 官方 esp_hidd profile（main/hid/ 下复用的官方代码），
 * 在其之上封装设备状态管理与便捷的报告发送接口。
 */
#include "hid_device.h"

#include <stdlib.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "esp_hidd_prf_api.h"
#include "hidd_le_prf_int.h"

/* 鼠标输入报告长度：必须等于 Report Map 里 mouse collection 的实际字节数
 *   buttons(3bit + 5bit padding) + X + Y + Wheel = 4 字节
 * 写成 5（官方 demo 的 AC Pan 版本）会让 macOS 判定报告长度不符，
 * 整包丢弃 —— 移动/滚轮/点击全部失效，且串口日志一切正常。 */
#define HID_MOUSE_IN_RPT_LEN 4

static const char *TAG = "hid_device";

#define HIDD_DEVICE_NAME    "BiliMouse"

/* 报告 ID（与官方 hid_device_le_prf.c 的报告表一致） */
#define RPT_ID_MOUSE         1
#define RPT_ID_KEYBOARD      2
#define RPT_ID_CONSUMER      3

/* ================= 状态 ================= */
static uint16_t s_conn_id = 0;
static bool     s_secure = false;         /* 已完成加密配对 */
static bool     s_connected = false;
static bool     s_report_enabled = false; /* 主机已订阅输入报告(CCCD) */
static hid_conn_state_t s_state = HID_STATE_IDLE;
static uint8_t  s_btn_mask = 0;           /* 当前按住的鼠标按键（左/右/中） */
static uint8_t  s_key_mod = 0;            /* 当前按住的键盘修饰键（Ctrl 等） */

static esp_bd_addr_t s_peer_bda;          /* 当前/最近连接的对方地址 */
static uint32_t s_connect_ms = 0;         /* 连接建立时刻，用于配对超时 */
static bool     s_adv_active = false;     /* 广播是否真的在跑（GAP 事件确认） */
static uint32_t s_adv_last_try_ms = 0;
static bool     s_pair_failed = false;    /* 最近一次配对失败（供 UI 提示） */
static uint8_t  s_auth_fail = 0;          /* 连续配对失败次数 */
static bool     s_ble_down = false;       /* BLE 协议栈已被挂起（OTA 期间） */

/* ================= 广播数据 ================= */
/*
 * 广播包总长上限 31 字节，这里只放 4 个字段：
 *   flags(3) + 服务UUID(4) + appearance(4) + 名字(11) = 22 字节
 *
 * ⚠️ 关键坑（曾导致「电脑搜不到 / 连不上」）：
 *   IDF 的 btc 层只接受 **128 位格式** 的 UUID 缓冲区。它固定读 16 字节，
 *   再按 BASE_UUID 逐字节比对，把其中「12 字节为 0 + 12/13 字节非 0」
 *   的判定为 16 位 UUID（见 btc_gatt_util.c: uuidType）。
 *   如果直接传一个 uint16_t（service_uuid_len = 2）：
 *     - 会越界读 15 个字节，uuidType 判定失败 → 广播里塞进一个
 *       128 位的垃圾 UUID，主机识别不到 0x1812 这个 HID 服务 → 拒绝连接
 *     - 且 btc 用 service_uuid_len/16*2 计算 malloc 长度 = 0，
 *       往 0 字节的堆块里写数据 → 堆破坏
 *   正确写法：填满 16 字节小端 128 位格式，btc 会自动压缩成 2 字节
 *   的 0x1812 写进广播包（占 4 字节）。
 */
static const uint8_t hid_service_uuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,   /* 00001812-0000-1000-8000-00805F9B34FB */
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = false,
    /* 必须为 0：非 0 会让 IDF 再塞一个 6 字节的「连接间隔范围」AD 结构，
       22 + 6 = 28，虽然还够，但纯属浪费广播空间且无实际作用 */
    .min_interval = 0,
    .max_interval = 0,
    /* 0x03C2 = HID Mouse（0x03C0 是 Generic HID）。
       macOS 会按 Appearance 决定挂哪个 HID profile / 显示什么图标，
       声明成 Generic 时辅助点按(右键)等鼠标专属行为可能不被启用。 */
    .appearance = ESP_BLE_APPEARANCE_HID_MOUSE,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hid_service_uuid),   /* 16：见上方说明，不能填 2 */
    .p_service_uuid = (uint8_t *)hid_service_uuid,
    .flag = 0x6,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = ESP_BLE_GAP_ADV_ITVL_MS(20),
    .adv_int_max = ESP_BLE_GAP_ADV_ITVL_MS(30),
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

/* ================= 工具 ================= */

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void start_advertising(void)
{
    s_adv_last_try_ms = now_ms();
    esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start advertising failed: %s", esp_err_to_name(err));
    }
}

/**
 * 向主机请求「短连接间隔」——鼠标点击能不能被识别，几乎全看这一步。
 *
 * ⚠️ 历史坑：这里以前用的是 esp_ble_gap_set_prefer_conn_params()。
 *    那个函数只是把参数写进 GAP 服务里的 PPCP 特征值（供主机来读），
 *    **不会对已建立的连接发起任何更新请求**。于是主机一直沿用自己
 *    想要的间隔（实测 macOS 会给到 30~45ms），而我们日志里看着像"已设置"。
 *    正确做法是 esp_ble_gap_update_conn_params()：主动发 L2CAP 参数更新请求。
 *
 * 参数取值按 Apple《Accessory Design Guidelines》：
 *   最小间隔 >= 15ms（12×1.25），最大间隔 >= 最小 + 15ms（24×1.25 = 30ms）
 *   latency = 0：按下和抬起必须落在相邻的连接事件里，跳过事件会让
 *   macOS 把 down/up 折叠成"什么都没发生"（点不动 / 双击变单击）。
 */
static void request_fast_conn_params(void)
{
    esp_ble_conn_update_params_t cp;
    memset(&cp, 0, sizeof(cp));
    memcpy(cp.bda, s_peer_bda, sizeof(esp_bd_addr_t));
    cp.min_int = 12;    /* 15.0ms */
    cp.max_int = 24;    /* 30.0ms */
    cp.latency = 0;
    cp.timeout = 400;   /* 4s：Apple 要求 >= 2s 且不能过长 */

    esp_err_t err = esp_ble_gap_update_conn_params(&cp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "conn param update request failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "requested conn interval 15~30ms (latency 0)");
    }
}

/* ================= 事件回调 ================= */

/** 主机订阅输入报告状态变化 */
static void on_report_notify(bool enabled)
{
    s_report_enabled = enabled;
    ESP_LOGI(TAG, "host input report notification %s", enabled ? "ENABLED" : "disabled");
    /* 订阅就绪后状态才真正变为 READY */
    if (enabled && s_connected && s_secure) {
        s_state = HID_STATE_READY;
    }
}

static void hidd_event_callback(esp_hidd_cb_event_t event, esp_hidd_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDD_EVENT_REG_FINISH:
        if (param->init_finish.state == ESP_HIDD_INIT_OK) {
            esp_ble_gap_set_device_name(HIDD_DEVICE_NAME);
            esp_ble_gap_config_adv_data(&s_adv_data);
            s_state = HID_STATE_ADVERTISING;
        }
        break;

    case ESP_HIDD_EVENT_DEINIT_FINISH:
        break;

    case ESP_HIDD_EVENT_BLE_CONNECT:
        ESP_LOGI(TAG, "BLE connected: %02x:%02x:%02x:%02x:%02x:%02x",
                 param->connect.remote_bda[0], param->connect.remote_bda[1],
                 param->connect.remote_bda[2], param->connect.remote_bda[3],
                 param->connect.remote_bda[4], param->connect.remote_bda[5]);
        memcpy(s_peer_bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        s_secure = false;
        s_report_enabled = false;
        s_adv_active = false;
        s_connect_ms = now_ms();
        s_pair_failed = false;
        s_state = HID_STATE_CONNECTING;

        /* 连接参数更新请求放到配对完成后再发（见 AUTH_CMPL）。
           刚连上就发，主机正忙着服务发现，多数会直接忽略。 */
        break;

    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, restarting advertising");
        s_connected = false;
        s_secure = false;
        s_report_enabled = false;
        s_adv_active = false;
        s_state = HID_STATE_ADVERTISING;
        start_advertising();
        break;

    case ESP_HIDD_EVENT_BLE_VENDOR_REPORT_WRITE_EVT:
        break;

    case ESP_HIDD_EVENT_BLE_LED_REPORT_WRITE_EVT:
        break;

    default:
        break;
    }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        if (param->adv_data_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "set adv data FAILED, status=0x%x（广播包可能超过 31 字节）",
                     param->adv_data_cmpl.status);
        } else {
            ESP_LOGI(TAG, "adv data set ok");
        }
        start_advertising();
        break;

    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            s_adv_active = true;
            ESP_LOGI(TAG, "advertising started as '%s'", HIDD_DEVICE_NAME);
        } else {
            s_adv_active = false;
            ESP_LOGE(TAG, "advertising start FAILED, status=0x%x",
                     param->adv_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        s_adv_active = false;
        break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        /* 记录主机最终接受的连接参数。这是排查"点击/拖拽丢失"的关键数据：
           若实际间隔很长（>50ms），一次短按下可能和松开落进同一个
           连接事件，被主机折叠成"什么都没发生"。 */
        ESP_LOGI(TAG, "conn params: interval=%.2fms (%d), latency=%d",
                 param->update_conn_params.conn_int * 1.25f,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "pairing success (encrypted link ready)");
            s_secure = true;
            s_auth_fail = 0;
            s_pair_failed = false;

            /* ⚠️ 这里必须把订阅状态补成 true（曾经「重连后全部按键失灵」的根因）：
               按 HCI/ATT 规范，绑定设备的 CCCD 订阅状态由从机保存，主机重连后
               不再重写 CCCD。只有「首次配对」时 macOS 才会写一次。
               于是重连后 s_report_enabled 一直是 false，
               hid_device_is_ready() 恒为假 —— 屏幕显示"已就绪"，
               但 click / move 全部在第一行就 return 了。
               加密完成即代表这是一条已绑定的可信链路，直接置位。 */
            s_report_enabled = true;
            s_state = HID_STATE_READY;

            /* 加密完成 = 主机已完成服务发现，此时请求短连接间隔才有意义 */
            request_fast_conn_params();
        } else {
            uint8_t reason = param->ble_security.auth_cmpl.fail_reason;
            ESP_LOGE(TAG, "pairing failed, reason=0x%x — 若电脑上还存着旧的配对记录，"
                          "请在系统蓝牙设置里移除 '%s' 后重新配对", reason, HIDD_DEVICE_NAME);
            s_secure = false;
            s_pair_failed = true;

            if (s_auth_fail < 255) {
                s_auth_fail++;
            }
            if (s_auth_fail >= 3) {
                ESP_LOGW(TAG, "连续 %d 次配对失败，清除本机保存的配对密钥", s_auth_fail);
                hid_device_clear_bonds();
                s_auth_fail = 0;
            }

            /* 对方很可能存着一把和本机不匹配的旧 LTK，会反复发起加密请求。
               不主动断开的话，现象就是「连上一秒就断、反复重连」。 */
            if (s_connected) {
                esp_ble_gap_disconnect(s_peer_bda);
            }
        }
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        /* 主机请求 PIN —— Just Works 模式下不会走到这里，兜底拒绝即可 */
        ESP_LOGW(TAG, "passkey requested by host, declining");
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
        break;

    case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
        /* 主机发来 passkey 供比对（MITM 模式）。Just Works 不走这里。 */
        ESP_LOGW(TAG, "passkey notification received");
        break;

    default:
        break;
    }
}

static esp_err_t ble_stack_start(void);

esp_err_t hid_device_init(void)
{
    esp_err_t ret;

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "nvs init failed");

    /* 仅启用 BLE，释放经典蓝牙内存 */
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    return ble_stack_start();
}

/**
 * BLE 协议栈整体启动（控制器 + Bluedroid + HID profile + 配对参数）。
 * 抽成独立函数：OTA 检查期间要先关掉蓝牙腾内存，结束后原样拉起来。
 */
static esp_err_t ble_stack_start(void)
{
    esp_err_t ret;

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "controller init failed");
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "controller enable failed");
        return ret;
    }

    esp_bluedroid_config_t cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ret = esp_bluedroid_init_with_cfg(&cfg);
    if (ret) {
        ESP_LOGE(TAG, "bluedroid init failed");
        return ret;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "bluedroid enable failed");
        return ret;
    }

    /* ⚠️ 顺序很重要：GAP 回调必须在 profile 初始化之前注册。
       esp_hidd_profile_init() 会在 GATT 注册完成后立刻回调 REG_FINISH，
       我们在那里配置广播数据；若回调注册晚了一步，
       「配置广播数据完成」事件就没人接，广播永远起不来 ——
       表现为电脑根本搜不到设备。 */
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gap_set_device_name(HIDD_DEVICE_NAME);

    ret = esp_hidd_profile_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hidd profile init failed");
        return ret;
    }

    esp_hidd_register_callbacks(hidd_event_callback);
    esp_hidd_register_report_notify_cb(on_report_notify);

    /* 配对参数。
     * 必须带 ESP_LE_AUTH_REQ_SC_ONLY（Secure Connections）——
     * Apple 设备（macOS / iOS）强制要求 LE SC，只用 Legacy Pairing
     * (ESP_LE_AUTH_BOND) 会导致能连接但配对不完成、输入报告发不出去。
     * ESP_IO_CAP_NONE = 无输入输出能力 -> 走 Just Works，免 PIN 码。 */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    const uint8_t *mac = esp_bt_dev_get_address();
    if (mac != NULL) {
        ESP_LOGI(TAG, "initialized, advertising as '%s', mac=%02x:%02x:%02x:%02x:%02x:%02x",
                 HIDD_DEVICE_NAME, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        ESP_LOGI(TAG, "initialized, advertising as '%s'", HIDD_DEVICE_NAME);
    }
    return ESP_OK;
}

hid_conn_state_t hid_device_state(void)
{
    return s_state;
}

/** 是否刚发生过配对失败（供界面给出可操作的提示） */
bool hid_device_pairing_failed(void)
{
    return s_pair_failed;
}

/**
 * 清除本机保存的所有配对密钥（bonds）。
 *
 * 用途：电脑端存着一把旧密钥、本机又对不上时，链路会「连上就断」。
 * 清掉本机记录后重新配对即可恢复。
 */
void hid_device_clear_bonds(void)
{
    int num = esp_ble_get_bond_device_num();
    if (num <= 0) {
        ESP_LOGI(TAG, "no bond record to clear");
        return;
    }
    if (num > 16) {
        num = 16;
    }

    esp_ble_bond_dev_t *list = calloc((size_t)num, sizeof(esp_ble_bond_dev_t));
    if (list == NULL) {
        ESP_LOGE(TAG, "out of memory while clearing bonds");
        return;
    }

    int n = num;
    if (esp_ble_get_bond_device_list(&n, list) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            esp_ble_remove_bond_device(list[i].bd_addr);
        }
        ESP_LOGW(TAG, "cleared %d bond(s)", n);
    } else {
        ESP_LOGE(TAG, "failed to read bond list");
    }
    free(list);
}

/* ================= OTA 期间的蓝牙挂起/恢复 =================
 * 内部 RAM 极度紧张：BLE（控制器 + Bluedroid 主机）常驻约 60~80KB，
 * 而 OTA 检查/下载需要 9KB 任务栈 + TLS 缓冲 + LWIP 收发缓冲，
 * 双方同时在场时 9KB 栈都分不出来（mem 显示 free 不足 15KB）。
 * OTA 流程先把蓝牙整个关掉 —— deinit 会把内存还给堆 —— 结束后再拉起。
 */
void hid_ble_suspend(void)
{
    if (s_ble_down) {
        return;
    }
    s_ble_down = true;
    s_connected = false;
    s_secure = false;
    s_report_enabled = false;
    s_state = HID_STATE_IDLE;
    s_adv_active = false;

    /* 先在 Bluedroid 还活着时注销 HID profile（GATT app unregister）。
       不做这一步，resume 时 esp_hidd_profile_init 会报
       "HID device profile already initialized"（内部静态标志没清），
       整个 resume 被判失败、广播永远起不来。 */
    esp_hidd_profile_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGW(TAG, "BLE suspended for OTA (heap returned)");
}

esp_err_t hid_ble_resume(void)
{
    if (!s_ble_down) {
        return ESP_OK;
    }
    esp_err_t err = ble_stack_start();
    if (err != ESP_OK) {
        s_ble_down = true;
        ESP_LOGE(TAG, "BLE resume failed: %s", esp_err_to_name(err));
        return err;
    }
    s_ble_down = false;
    ESP_LOGI(TAG, "BLE resumed");
    return ESP_OK;
}

/**
 * 是否可以发送 HID 报告。
 *
 * 判定条件（三者都要满足）：
 *   1. 已连接
 *   2. 链路已加密（macOS 强制要求 LE SC）
 *   3. 主机已订阅输入报告（CCCD 已使能）
 *
 * 只满足 1、2 时主机仍可能收不到报告 —— 那是「连上了却没反应」的
 * 最隐蔽的一种情况，单独用 s_report_enabled 把它区分出来。
 */
bool hid_device_is_ready(void)
{
    return !s_ble_down && s_connected && s_secure && s_report_enabled;
}

/** 主机是否已订阅输入报告（用于屏幕上的诊断提示） */
bool hid_device_report_enabled(void)
{
    return s_report_enabled;
}

/**
 * 蓝牙状态巡检（10Hz 左右由主循环调用）
 *
 * 两件事：
 *   1. 广播看门狗 —— 广播可能因为时序/协议栈原因悄悄没起来，
 *      这时设备在电脑上「根本不存在」，用户只会觉得「连不上」。
 *   2. 配对超时 —— 连上了 8 秒还没加密，说明双方密钥对不上，
 *      主动断开让主机重新发起配对，否则会卡在「连接中」永远不动。
 */
void hid_device_service(void)
{
    uint32_t now = now_ms();

    if (s_ble_down) {
        return;   /* OTA 期间协议栈已关，广播看门狗不要去碰 GAP API */
    }
    if (!s_connected && !s_adv_active && (now - s_adv_last_try_ms) > 3000) {
        ESP_LOGW(TAG, "advertising is not running, restarting");
        start_advertising();
    }

    if (s_connected && !s_secure && (now - s_connect_ms) > 8000) {
        ESP_LOGW(TAG, "no encryption within 8s, disconnecting so host can retry");
        esp_ble_gap_disconnect(s_peer_bda);
        s_connect_ms = now;
    }
}

/* 合成点击正在进行（按下尚未松开）—— 这段时间必须禁止任何位移，
   否则 macOS 会把这次点击判成 0 像素的拖拽，单击/双击全部失效。 */
static volatile bool s_click_busy = false;

/* ================= 鼠标 ================= */
esp_err_t hid_mouse_move(int8_t dx, int8_t dy)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_click_busy) {
        /* 点击窗口（<50ms）内的位移直接丢弃：丢几帧手感几乎看不出来，
           但混进一次 dx!=0 就会让整次点击变成拖拽。 */
        return ESP_OK;
    }
    /* 带上当前按住的按键状态：否则拖动时主机认为按键已松开，
       表现为「按住拖不动 / 拖到一半选中被取消」。 */
    esp_hidd_send_mouse_value(s_conn_id, s_btn_mask, dx, dy);
    return ESP_OK;
}

/**
 * 按住 / 松开鼠标按键（不自动释放）。
 *
 * 这是把实体按键当成真实鼠标左键的关键：
 *   - 按下 -> 主机立即收到「左键按下」，可以开始框选、拖动窗口
 *   - 松开 -> 主机收到「左键松开」
 * 单击 / 双击 / 连击全部交给主机的系统级判定，不需要固件自己计时，
 * 也就不会出现「双击窗口设得太短点不出来」的问题。
 */
esp_err_t hid_mouse_button(uint8_t button, bool down)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (down) {
        s_btn_mask |= button;
    } else {
        s_btn_mask &= (uint8_t)~button;
    }
    esp_err_t err = esp_hidd_send_mouse_value(s_conn_id, s_btn_mask, 0, 0);
    ESP_LOGI(TAG, "mouse btn 0x%02x %s (mask=0x%02x, %s)", button,
             down ? "down" : "up", s_btn_mask, esp_err_to_name(err));
    return ESP_OK;
}

/** 释放所有按键（断连/切模式时兜底，避免主机卡在"按下"状态） */
void hid_mouse_release_all(void)
{
    /* 键盘修饰键也一并释放：实体右键靠 Ctrl+左键 模拟，
       断连/切模式时如果 Ctrl 卡在按住状态，主机所有点击都会变成右键 */
    if (s_key_mod != 0) {
        s_key_mod = 0;
        esp_hidd_send_keyboard_value(s_conn_id, 0, NULL, 0);
    }
    if (s_btn_mask == 0) {
        return;
    }
    s_btn_mask = 0;
    if (hid_device_is_ready()) {
        esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0);
    }
}

/**
 * 按住 / 松开键盘修饰键（不自动释放，不按键）。
 *
 * 实体右键的实现方式：macOS 全局把 Control+左键 视为右键，
 * 且该规则不依赖"系统识别出鼠标按钮 2"——很多 macOS 版本对
 * 第三方 BLE 鼠标的右键（按钮 2）根本不响应，也没提供设置入口，
 * Ctrl+左键 是唯一 100% 可靠的模拟方式。
 */
esp_err_t hid_keyboard_modifier(uint8_t modifier, bool down)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (down) {
        s_key_mod |= modifier;
    } else {
        s_key_mod &= (uint8_t)~modifier;
    }
    esp_hidd_send_keyboard_value(s_conn_id, s_key_mod, NULL, 0);
    return ESP_OK;
}
esp_err_t hid_mouse_scroll(int8_t wheel)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 官方 esp_hidd_send_mouse_value 把滚轮字段硬编码为 0，
       这里直接构造报告，走同一个 Report characteristic。 */
    uint8_t buffer[HID_MOUSE_IN_RPT_LEN];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0;        /* 无按键 */
    buffer[1] = 0;        /* X */
    buffer[2] = 0;        /* Y */
    buffer[3] = (uint8_t)wheel;  /* Wheel */

    hid_dev_send_report(hidd_le_env.gatt_if, s_conn_id,
                        HID_RPT_ID_MOUSE_IN, HID_REPORT_TYPE_INPUT,
                        HID_MOUSE_IN_RPT_LEN, buffer);
    return ESP_OK;
}

/**
 * 单击时按键按住的时长（毫秒）。
 *
 * 原来是 120ms，那是「双击永远出不来的」主要原因之一：
 *   第1次 down(0) -> up(120) -> 手指抬起再按(约 +120ms) -> 第2次 down(240)
 *   -> up(360)，两次点击的节奏被拉长到 350ms 以上，macOS 直接判成
 *   两次独立单击。
 * 45ms 足以跨越 2~3 个连接事件（间隔 15~30ms），down 和 up 不会被折叠，
 * 又不会拖慢双击节奏。
 */
#define CLICK_HOLD_MS 45

esp_err_t hid_mouse_click(uint8_t button)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_click_busy) {
        return ESP_OK;   /* 上一次点击还没发完，合并掉，避免 down/down/up 乱序 */
    }
    s_click_busy = true;

    /*
     * 一次可靠的单击 = 纯按键报告 down -> 保持 -> up，零位移。
     *
     * 关键点（历史上每一环都踩过）：
     *   1. 报告长度必须等于描述符声明的字节数（本工程 4 字节）。
     *   2. down 必须同步写进 s_btn_mask：否则点击期间任何一次
     *      hid_mouse_move() 都会带着 mask=0 发出去，等于把按键提前松开
     *      （主机收到 up，后面的 up 就变成无效包 -> 点了没反应）。
     *   3. 不能带位移：Apple 把"按住期间有位移"归为拖拽。
     *      现在由 s_click_busy 在 hid_mouse_move() 里整体拦掉。
     */
    uint8_t prev = s_btn_mask;
    s_btn_mask |= button;
    esp_hidd_send_mouse_value(s_conn_id, s_btn_mask, 0, 0);

    vTaskDelay(pdMS_TO_TICKS(CLICK_HOLD_MS));

    s_btn_mask = (uint8_t)(prev & (uint8_t)~button);
    esp_hidd_send_mouse_value(s_conn_id, s_btn_mask, 0, 0);

    s_click_busy = false;
    ESP_LOGI(TAG, "mouse click btn=0x%02x (%dms)", button, CLICK_HOLD_MS);
    return ESP_OK;
}

esp_err_t hid_mouse_move_relative_steps(int32_t total_dx, int32_t total_dy, int steps)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (steps <= 0) {
        steps = 8;
    }
    /* 单步位移限制在 ±120，避免被主机识别为瞬移 */
    const int32_t max_step = 120;
    int32_t step_dx = total_dx / steps;
    int32_t step_dy = total_dy / steps;
    if (step_dx > max_step)  step_dx = max_step;
    if (step_dx < -max_step) step_dx = -max_step;
    if (step_dy > max_step)  step_dy = max_step;
    if (step_dy < -max_step) step_dy = -max_step;

    for (int i = 0; i < steps; i++) {
        int32_t dx = step_dx;
        int32_t dy = step_dy;
        /* 最后一步补足余量 */
        if (i == steps - 1) {
            dx = total_dx - step_dx * (steps - 1);
            dy = total_dy - step_dy * (steps - 1);
            if (dx > 127)  dx = 127;
            if (dx < -127) dx = -127;
            if (dy > 127)  dy = 127;
            if (dy < -127) dy = -127;
        }
        esp_hidd_send_mouse_value(s_conn_id, 0, (int8_t)dx, (int8_t)dy);
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    return ESP_OK;
}

/* ================= 键盘 ================= */
esp_err_t hid_keyboard_combo(uint8_t keycode, uint8_t modifier)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t keys[1] = { keycode };
    /* 按下。保持 60ms 而不是 30ms：主机同样会折叠靠得太近的报告，
       实体键只按一次却"没反应"多数是这个原因。 */
    esp_hidd_send_keyboard_value(s_conn_id, modifier, keys, 1);
    vTaskDelay(pdMS_TO_TICKS(60));
    /* 释放 */
    esp_hidd_send_keyboard_value(s_conn_id, 0, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t hid_keyboard_tap(uint8_t keycode, uint8_t modifier)
{
    return hid_keyboard_combo(keycode, modifier);
}

/* ================= 消费类 ================= */
esp_err_t hid_consumer_tap(uint8_t cmd)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_hidd_send_consumer_value(s_conn_id, cmd, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_hidd_send_consumer_value(s_conn_id, cmd, false);
    return ESP_OK;
}
