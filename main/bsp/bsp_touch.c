#include "bsp_touch.h"
#include "bsp_board.h"
#include "bsp_i2c.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_touch";

/* FT6336 寄存器 */
#define FT_REG_DEVICE_MODE   0x00
#define FT_REG_GESTURE_ID     0x01
#define FT_REG_TD_STATUS     0x02
#define FT_REG_TOUCH1_XH     0x03
#define FT_REG_CHIP_ID       0xA3

/* 触摸面板原始分辨率（12bit 坐标空间 0..4095）对应的物理尺寸 */
#define FT_PANEL_MAX_X       320
#define FT_PANEL_MAX_Y       240

static i2c_master_dev_handle_t s_dev;
static bool s_ready = false;

static esp_err_t touch_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

static esp_err_t touch_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

esp_err_t bsp_touch_init(void)
{
    /* 挂到共享 I2C 总线上（总线由 bsp_i2c 统一管理） */
    ESP_RETURN_ON_ERROR(bsp_i2c_add_device(BSP_FT6336_ADDR, &s_dev), TAG, "add device failed");

    /* 切到正常工作模式 */
    esp_err_t err = touch_write(FT_REG_DEVICE_MODE, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 not responding at 0x%02X", BSP_FT6336_ADDR);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t chip_id = 0;
    touch_read(FT_REG_CHIP_ID, &chip_id, 1);
    ESP_LOGI(TAG, "initialized (chip_id=0x%02X)", chip_id);
    s_ready = true;
    return ESP_OK;
}

esp_err_t bsp_touch_read(bsp_touch_data_t *data)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buf[5];
    esp_err_t err = touch_read(FT_REG_TD_STATUS, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;   /* 由调用方限流打印，避免 50Hz 刷屏饿死蓝牙任务 */
    }

    uint8_t td_status = buf[0];
    data->pressed = false;
    data->x = 0;
    data->y = 0;

    /* 低 4bit = 第 1 个触点状态，非 0 表示有触摸 */
    if ((td_status & 0x0F) == 0) {
        return ESP_OK;
    }

    /* buf[1]=XH, buf[2]=XL, buf[3]=YH, buf[4]=YL */
    uint16_t raw_x = (uint16_t)(((buf[1] & 0x0F) << 8) | buf[2]);
    uint16_t raw_y = (uint16_t)(((buf[3] & 0x0F) << 8) | buf[4]);

    /* 面板坐标 -> 屏幕坐标。面板为竖装 320x240，
       屏幕横屏 swap_xy 后需做一次 X/Y 交换与方向翻转。 */
    uint16_t px = raw_x * FT_PANEL_MAX_X / 4096;
    uint16_t py = raw_y * FT_PANEL_MAX_Y / 4096;

    /* 面板原始 X 对应屏幕 Y（旋转 90°），并翻转以匹配 LCD 方向 */
    data->x = py;
    data->y = (FT_PANEL_MAX_Y - 1) - px;
    if (data->x >= FT_PANEL_MAX_X) {
        data->x = FT_PANEL_MAX_X - 1;
    }
    if (data->y >= FT_PANEL_MAX_Y) {
        data->y = FT_PANEL_MAX_Y - 1;
    }
    data->pressed = true;
    return ESP_OK;
}
