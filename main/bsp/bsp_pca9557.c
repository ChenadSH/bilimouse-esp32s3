#include "bsp_pca9557.h"
#include "bsp_board.h"
#include "bsp_i2c.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_pca9557";

/* PCA9557 寄存器 */
#define PCA9557_REG_INPUT   0x00
#define PCA9557_REG_OUTPUT  0x01
#define PCA9557_REG_POLARITY 0x02
#define PCA9557_REG_CONFIG  0x03

static i2c_master_dev_handle_t s_dev;

static esp_err_t pca9557_write(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), pdMS_TO_TICKS(100));
}

static esp_err_t pca9557_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

esp_err_t bsp_pca9557_init(void)
{
    /* 总线由 bsp_i2c 统一管理，这里只挂自己的设备 */
    ESP_RETURN_ON_ERROR(bsp_i2c_add_device(BSP_PCA9557_ADDR, &s_dev),
                        TAG, "add device failed");

    /* IO0=LCD_CS 初始拉高(禁用)，IO1=PA_EN 拉低，IO2=DVP_PWDN 拉高(摄像头关闭) */
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_OUTPUT, 0x05), TAG, "output init failed");
    /* IO0~IO2 配置为输出(0)，IO3~IO7 保持输入(1) */
    ESP_RETURN_ON_ERROR(pca9557_write(PCA9557_REG_CONFIG, 0xF8), TAG, "config failed");

    ESP_LOGI(TAG, "initialized (IO0=LCD_CS, IO1=PA_EN, IO2=DVP_PWDN)");
    return ESP_OK;
}

esp_err_t bsp_pca9557_set_pin(uint8_t bit, bool level)
{
    uint8_t data;
    ESP_RETURN_ON_ERROR(pca9557_read(PCA9557_REG_OUTPUT, &data), TAG, "read failed");
    data = (uint8_t)((data & ~(1u << bit)) | ((level ? 1u : 0u) << bit));
    return pca9557_write(PCA9557_REG_OUTPUT, data);
}

esp_err_t bsp_pca9557_get_pin(uint8_t bit, bool *level)
{
    uint8_t data;
    ESP_RETURN_ON_ERROR(pca9557_read(PCA9557_REG_INPUT, &data), TAG, "read failed");
    *level = (data & (1u << bit)) != 0;
    return ESP_OK;
}

esp_err_t bsp_pca9557_release_lcd(void)
{
    ESP_RETURN_ON_ERROR(bsp_pca9557_set_pin(BSP_PCA9557_LCD_CS_BIT, false), TAG, "release lcd cs failed");
    ESP_LOGI(TAG, "LCD_CS released (IO0 low), panel out of reset");
    return ESP_OK;
}
