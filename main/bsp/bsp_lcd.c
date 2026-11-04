#include "bsp_lcd.h"
#include "bsp_board.h"
#include "bsp_pca9557.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_st7789.h"

static const char *TAG = "bsp_lcd";

#define LCD_BL_LEDC_CH   LEDC_CHANNEL_0
#define LCD_BL_LEDC_TIMER LEDC_TIMER_0
#define LCD_BL_LEDC_MODE LEDC_LOW_SPEED_MODE
#define LCD_BL_LEDC_RES  LEDC_TIMER_8_BIT

static bool s_bl_configured;

static esp_err_t lcd_backlight_init(void)
{
    if (s_bl_configured) {
        return ESP_OK;
    }

    ledc_timer_config_t timer = {
        .speed_mode = LCD_BL_LEDC_MODE,
        .duty_resolution = LCD_BL_LEDC_RES,
        .timer_num = LCD_BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer failed");

    ledc_channel_config_t ch = {
        .gpio_num = BSP_LCD_PIN_BL,
        .speed_mode = LCD_BL_LEDC_MODE,
        .channel = LCD_BL_LEDC_CH,
        .timer_sel = LCD_BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        /* 背光低电平点亮 → 输出取反 */
        .flags.output_invert = true,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel failed");

    s_bl_configured = true;
    return ESP_OK;
}

esp_err_t bsp_lcd_set_backlight(uint8_t percent)
{
    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)((percent * 255) / 100);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CH, duty), TAG, "set duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CH), TAG, "update duty failed");
    ESP_LOGI(TAG, "backlight %u%%", percent);
    return ESP_OK;
}

esp_err_t bsp_lcd_init(bsp_lcd_t *lcd)
{
    ESP_RETURN_ON_ERROR(bsp_pca9557_init(), TAG, "pca9557 init failed");
    ESP_RETURN_ON_ERROR(lcd_backlight_init(), TAG, "backlight init failed");

    /* 先关背光，避免屏上出现花屏雪花 */
    ESP_RETURN_ON_ERROR(ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CH, 0), TAG, "bl off failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CH), TAG, "bl off failed");

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = BSP_LCD_PIN_SCLK,
        .mosi_io_num = BSP_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BSP_LCD_HOR_RES * 40 * (int)sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = -1,   /* 片选由 PCA9557 IO0 控制 */
        .dc_gpio_num = BSP_LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &io_cfg, &lcd->io),
                        TAG, "panel io failed");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BSP_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(lcd->io, &panel_cfg, &lcd->panel),
                        TAG, "panel create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(lcd->panel), TAG, "panel reset failed");
    /* 关键：先释放 LCD_CS，屏幕才退出复位态 */
    ESP_RETURN_ON_ERROR(bsp_pca9557_release_lcd(), TAG, "release cs failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(lcd->panel), TAG, "panel init failed");

    /* 横屏 320x240 的真机验证参数 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(lcd->panel, true), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(lcd->panel, true), TAG, "swap failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(lcd->panel, true, false), TAG, "mirror failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(lcd->panel, true), TAG, "disp on failed");

    lcd->width = BSP_LCD_HOR_RES;
    lcd->height = BSP_LCD_VER_RES;

    ESP_LOGI(TAG, "initialized: %dx%d", lcd->width, lcd->height);
    return ESP_OK;
}

esp_err_t bsp_lcd_draw_bitmap(bsp_lcd_t *lcd, int x1, int y1, int x2, int y2, const uint16_t *data)
{
    return esp_lcd_panel_draw_bitmap(lcd->panel, x1, y1, x2, y2, data);
}
