#include "bsp_button.h"
#include "bsp_board.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp_button";

#define DEBOUNCE_MS      20
#define POLL_INTERVAL_MS 5

static volatile bool s_changed;
static bsp_button_state_t s_state;

static void IRAM_ATTR button_isr(void *arg)
{
    s_changed = true;
}

/** 读取原始电平并做稳定时间判定 */
static bsp_button_state_t raw_state(void)
{
    int level = gpio_get_level(BSP_BUTTON_GPIO);
    return (BSP_BUTTON_ACTIVE_LOW && level == 0) ? BSP_BUTTON_PRESSED : BSP_BUTTON_RELEASED;
}

esp_err_t bsp_button_init(void)
{
    esp_err_t err = gpio_config(&(gpio_config_t){
        .pin_bit_mask = 1ULL << BSP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    });
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed: %s", esp_err_to_name(err));
        return err;
    }

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BSP_BUTTON_GPIO, button_isr, NULL);

    s_state = raw_state();
    ESP_LOGI(TAG, "initialized (gpio %d, active_low=%d, state=%s)",
             BSP_BUTTON_GPIO, BSP_BUTTON_ACTIVE_LOW,
             s_state == BSP_BUTTON_PRESSED ? "pressed" : "released");
    return ESP_OK;
}

bsp_button_state_t bsp_button_get_state(void)
{
    bsp_button_state_t now = raw_state();
    if (now != s_state) {
        /* 需要连续稳定 DEBOUNCE_MS 才认可状态变化 */
        int64_t deadline = esp_timer_get_time() + (int64_t)DEBOUNCE_MS * 1000;
        while (esp_timer_get_time() < deadline) {
            if (raw_state() != now) {
                return s_state;   /* 抖动，放弃本次变化 */
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        s_state = now;
        s_changed = true;
    }
    return s_state;
}

bsp_button_state_t bsp_button_wait_change(bsp_button_state_t last, uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_changed) {
            s_changed = false;
            bsp_button_state_t cur = bsp_button_get_state();
            if (cur != last) {
                return cur;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
    return last;
}
