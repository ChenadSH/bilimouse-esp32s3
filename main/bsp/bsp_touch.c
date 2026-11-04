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

/* 触摸单独走 100kHz：FT6x36 和 4 个 I2C 从设备共用总线时，
   400kHz 下多字节读很容易只回第一字节、后面全是 0 */
#define FT_I2C_FREQ_HZ       100000

static i2c_master_dev_handle_t s_dev;
static bool s_ready = false;

/* 读坐标的两种方式，运行时自适应切换 */
typedef enum {
    RD_BURST = 0,   /* 一次性读 0x02..0x06（状态 + 坐标同一个事务） */
    RD_SPLIT,       /* 先读状态，再单独读 0x03..0x06 */
} rd_mode_t;

static rd_mode_t s_mode = RD_BURST;
static uint8_t   s_zero_hits = 0;   /* 连续"按下但坐标为 0"的次数 */

static esp_err_t touch_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, pdMS_TO_TICKS(100));
}

static esp_err_t touch_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

/** 建立/重建 I2C 设备句柄（切换总线速率时用） */
static esp_err_t touch_dev_attach(uint32_t freq_hz)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }

    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_bus_get(&bus), TAG, "get bus failed");

    return i2c_master_bus_add_device(bus, &(i2c_device_config_t){
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BSP_FT6336_ADDR,
        .scl_speed_hz = freq_hz,
    }, &s_dev);
}

esp_err_t bsp_touch_init(void)
{
    ESP_RETURN_ON_ERROR(touch_dev_attach(FT_I2C_FREQ_HZ), TAG, "add device failed");

    /* 切到正常工作模式 */
    esp_err_t err = touch_write(FT_REG_DEVICE_MODE, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FT6336 not responding at 0x%02X", BSP_FT6336_ADDR);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t chip_id = 0;
    touch_read(FT_REG_CHIP_ID, &chip_id, 1);
    ESP_LOGI(TAG, "initialized (chip_id=0x%02X, i2c %dkHz)",
             chip_id, FT_I2C_FREQ_HZ / 1000);
    s_ready = true;
    s_mode = RD_BURST;
    s_zero_hits = 0;
    return ESP_OK;
}

esp_err_t bsp_touch_dump(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t a[16] = {0};
    uint8_t b[16] = {0};
    touch_read(0x00, a, sizeof(a));
    touch_read(0x02, b, sizeof(b));

    ESP_LOGW(TAG, "regs@00: %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x",
             a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7],
             a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
    ESP_LOGW(TAG, "regs@02: %02x %02x %02x %02x %02x %02x",
             b[0], b[1], b[2], b[3], b[4], b[5]);
    return ESP_OK;
}

/** 按下但坐标全 0：换一种读法再试，同时把寄存器打出来 */
static void on_zero_coords(void)
{
    s_zero_hits++;

    if (s_zero_hits == 3) {
        s_mode = RD_SPLIT;
        ESP_LOGW(TAG, "coords always 0 -> switching to split read");
        bsp_touch_dump();
    } else if (s_zero_hits == 6) {
        /* 两种读法都不行，最后试一次重配工作模式 */
        ESP_LOGE(TAG, "still no coordinates, re-initializing chip");
        bsp_touch_dump();
        touch_write(FT_REG_DEVICE_MODE, 0x00);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_zero_hits = 0;
    }
}

esp_err_t bsp_touch_read(bsp_touch_data_t *data)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    data->pressed = false;
    data->raw_x = 0;
    data->raw_y = 0;

    uint8_t status = 0;
    uint8_t xy[4] = {0};

    if (s_mode == RD_BURST) {
        uint8_t buf[5] = {0};
        esp_err_t err = touch_read(FT_REG_TD_STATUS, buf, sizeof(buf));
        if (err != ESP_OK) {
            return err;
        }
        status = buf[0];
        xy[0] = buf[1];
        xy[1] = buf[2];
        xy[2] = buf[3];
        xy[3] = buf[4];
    } else {
        esp_err_t err = touch_read(FT_REG_TD_STATUS, &status, 1);
        if (err != ESP_OK) {
            return err;
        }
        if ((status & 0x0F) == 0) {
            return ESP_OK;
        }
        err = touch_read(FT_REG_TOUCH1_XH, xy, sizeof(xy));
        if (err != ESP_OK) {
            return err;
        }
    }

    if ((status & 0x0F) == 0) {
        return ESP_OK;
    }

    data->raw_x = (uint16_t)(((xy[0] & 0x0F) << 8) | xy[1]);
    data->raw_y = (uint16_t)(((xy[2] & 0x0F) << 8) | xy[3]);
    data->pressed = true;

    if (data->raw_x == 0 && data->raw_y == 0) {
        on_zero_coords();
    } else {
        s_zero_hits = 0;
    }

    return ESP_OK;
}
