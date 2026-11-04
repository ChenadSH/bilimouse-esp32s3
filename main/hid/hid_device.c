/**
 * @file hid_device.c
 * @brief BiliMouse BLE HID 应用层实现
 *
 * 基于 ESP-IDF 官方 esp_hidd profile（main/hid/ 下复用的官方代码），
 * 在其之上封装设备状态管理与便捷的报告发送接口。
 */
#include "hid_device.h"

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
static bool     s_secure = false;       /* 已完成加密配对 */
static bool     s_connected = false;
static hid_conn_state_t s_state = HID_STATE_IDLE;

/* ================= 广播数据 ================= */
static uint8_t hidd_service_uuid128[] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x12, 0x18, 0x00, 0x00,
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = ESP_BLE_GAP_CONN_ITVL_MS(7.5),
    .max_interval = ESP_BLE_GAP_CONN_ITVL_MS(20),
    .appearance = 0x03c0,      /* HID Generic */
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = sizeof(hidd_service_uuid128),
    .p_service_uuid = hidd_service_uuid128,
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

/* ================= 事件回调 ================= */
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
        ESP_LOGI(TAG, "BLE connected");
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        s_secure = false;
        s_state = HID_STATE_CONNECTING;
        break;

    case ESP_HIDD_EVENT_BLE_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, restarting advertising");
        s_connected = false;
        s_secure = false;
        s_state = HID_STATE_ADVERTISING;
        esp_ble_gap_start_advertising(&s_adv_params);
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
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "pairing success, secure connection established");
            s_secure = true;
            s_state = HID_STATE_READY;
        } else {
            ESP_LOGE(TAG, "pairing failed, reason = 0x%x",
                     param->ble_security.auth_cmpl.fail_reason);
            s_secure = false;
        }
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

    ret = esp_hidd_profile_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "hidd profile init failed");
        return ret;
    }

    esp_ble_gap_register_callback(gap_event_handler);
    esp_hidd_register_callbacks(hidd_event_callback);

    /* 配对参数：无输入无输出 IO 能力（Windows 下自动接受配对） */
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;

    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(uint8_t));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(uint8_t));

    ESP_LOGI(TAG, "initialized, advertising as '%s'", HIDD_DEVICE_NAME);
    return ESP_OK;
}

hid_conn_state_t hid_device_state(void)
{
    return s_state;
}

bool hid_device_is_ready(void)
{
    return s_connected && s_secure;
}

/* ================= 鼠标 ================= */
esp_err_t hid_mouse_move(int8_t dx, int8_t dy)
{
    if (!hid_device_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    mouse_cmd_t cmd = 0;
    esp_hidd_send_mouse_value(s_conn_id, cmd, dx, dy);
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
