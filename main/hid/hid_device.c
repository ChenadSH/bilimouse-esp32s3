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

/* 官方 esp_hidd_prf_api.c 中的局部宏，此处复用以保持报告长度一致 */
#define HID_MOUSE_IN_RPT_LEN 5

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

static esp_bd_addr_t s_peer_bda;          /* 当前/最近连接的对方地址 */
static uint32_t s_connect_ms = 0;         /* 连接建立时刻，用于配对超时 */
static bool     s_adv_active = false;     /* 广播是否真的在跑（GAP 事件确认） */
static uint32_t s_adv_last_try_ms = 0;
static bool     s_pair_failed = false;    /* 最近一次配对失败（供 UI 提示） */
static uint8_t  s_auth_fail = 0;          /* 连续配对失败次数 */

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
    .appearance = 0x03c0,      /* HID Generic */
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

        /* 鼠标要求低延迟；主机可以拒绝，失败不影响功能 */
        esp_ble_gap_set_prefer_conn_params(s_peer_bda,
                                          ESP_BLE_GAP_CONN_ITVL_MS(7.5),
                                          ESP_BLE_GAP_CONN_ITVL_MS(15),
                                          0,
                                          ESP_BLE_GAP_SUPERVISION_TIMEOUT_MS(4000));
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

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "pairing success (encrypted link ready)");
            s_secure = true;
            s_auth_fail = 0;
            s_pair_failed = false;
            s_state = HID_STATE_READY;
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
    return s_connected && s_secure && s_report_enabled;
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

/* ================= 鼠标 ================= */
esp_err_t hid_mouse_move(int8_t dx, int8_t dy)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
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
    esp_hidd_send_mouse_value(s_conn_id, s_btn_mask, 0, 0);
    return ESP_OK;
}

/** 释放所有按键（断连/切模式时兜底，避免主机卡在"按下"状态） */
void hid_mouse_release_all(void)
{
    if (s_btn_mask == 0) {
        return;
    }
    s_btn_mask = 0;
    if (hid_device_is_ready()) {
        esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0);
    }
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
    buffer[4] = 0;        /* AC Pan */

    hid_dev_send_report(hidd_le_env.gatt_if, s_conn_id,
                        HID_RPT_ID_MOUSE_IN, HID_REPORT_TYPE_INPUT,
                        HID_MOUSE_IN_RPT_LEN, buffer);
    return ESP_OK;
}

esp_err_t hid_mouse_click(uint8_t button)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* 按下 */
    esp_hidd_send_mouse_value(s_conn_id, button, 0, 0);
    vTaskDelay(pdMS_TO_TICKS(30));
    /* 释放 */
    esp_hidd_send_mouse_value(s_conn_id, 0, 0, 0);
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
    /* 按下 */
    esp_hidd_send_keyboard_value(s_conn_id, modifier, keys, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    /* 释放 */
    esp_hidd_send_keyboard_value(s_conn_id, 0, NULL, 0);
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
