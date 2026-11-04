#include "bsp_i2c.h"

#include "bsp_board.h"

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "bsp_i2c";

static i2c_master_bus_handle_t s_bus = NULL;

esp_err_t bsp_i2c_bus_get(i2c_master_bus_handle_t *bus)
{
    if (s_bus) {
        if (bus) {
            *bus = s_bus;
        }
        return ESP_OK;
    }

    esp_err_t err = i2c_new_master_bus(&(i2c_master_bus_config_t){
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    }, &s_bus);

    if (err != ESP_OK) {
        /* 失败时 s_bus 未被赋值，必须清零，否则后续会用到野句柄 */
        s_bus = NULL;
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "i2c bus created (port %d, sda %d, scl %d)",
             BSP_I2C_PORT, BSP_I2C_SDA, BSP_I2C_SCL);
    if (bus) {
        *bus = s_bus;
    }
    return ESP_OK;
}

esp_err_t bsp_i2c_add_device(uint16_t addr, i2c_master_dev_handle_t *out)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(bsp_i2c_bus_get(&bus), TAG, "get bus failed");

    return i2c_master_bus_add_device(bus, &(i2c_device_config_t){
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = BSP_I2C_FREQ_HZ,
    }, out);
}

bool bsp_i2c_probe(uint16_t addr)
{
    i2c_master_bus_handle_t bus = NULL;
    if (bsp_i2c_bus_get(&bus) != ESP_OK) {
        return false;
    }
    esp_err_t err = i2c_master_probe(bus, addr, 100);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "probe 0x%02X failed: %s", addr, esp_err_to_name(err));
        return false;
    }
    return true;
}
