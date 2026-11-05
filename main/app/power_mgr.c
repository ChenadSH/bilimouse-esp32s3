#include "power_mgr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_lcd.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "power";

/* 30s 无人操作 -> 变暗；再等 60s -> 息屏。
   躺着看视频时手会停在屏幕上，触摸本身也会续命。 */
#define DIM_AFTER_MS      30000
#define SLEEP_AFTER_MS    90000
#define DIM_BRIGHTNESS_PCT 25

static volatile uint32_t s_last_activity_ms;
static power_state_t     s_state = POWER_ACTIVE;
static uint8_t           s_normal_bl = 80;
static bool              s_inited;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void apply_backlight(uint8_t percent)
{
    bsp_lcd_set_backlight(percent);
}

static void enter_state(power_state_t st)
{
    if (s_state == st) {
        return;
    }
    s_state = st;

    switch (st) {
    case POWER_ACTIVE:
        apply_backlight(s_normal_bl);
        ui_mark_dirty();
        ESP_LOGI(TAG, "screen on (%u%%)", s_normal_bl);
        break;

    case POWER_DIM: {
        uint8_t bl = (uint8_t)((s_normal_bl * DIM_BRIGHTNESS_PCT) / 100);
        if (bl < 5) {
            bl = 5;
        }
        apply_backlight(bl);
        ESP_LOGI(TAG, "screen dim (%u%%)", bl);
        break;
    }

    case POWER_SLEEP:
        apply_backlight(0);
        ESP_LOGI(TAG, "screen off (power save)");
        break;
    }
}

esp_err_t power_mgr_init(void)
{
    const settings_t *cfg = settings_get();
    if (cfg && cfg->backlight <= 100 && cfg->backlight >= 10) {
        s_normal_bl = cfg->backlight;
    }
    s_last_activity_ms = now_ms();
    s_state = POWER_ACTIVE;
    s_inited = true;
    apply_backlight(s_normal_bl);
    ESP_LOGI(TAG, "init: brightness=%u%%, dim@%dms, off@%dms",
             s_normal_bl, DIM_AFTER_MS, SLEEP_AFTER_MS);
    return ESP_OK;
}

void power_mgr_notify_activity(void)
{
    s_last_activity_ms = now_ms();
    if (s_state != POWER_ACTIVE) {
        /* 息屏时任何触摸/按键都应该立刻点亮，并把界面重画一次 */
        enter_state(POWER_ACTIVE);
    }
}

bool power_mgr_display_on(void)
{
    return s_state != POWER_SLEEP;
}

power_state_t power_mgr_state(void)
{
    return s_state;
}

void power_mgr_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (percent < 10) {
        percent = 10;   /* 低于 10% 在强光下完全看不见，没有实用价值 */
    }
    s_normal_bl = percent;

    settings_t *cfg = settings_mut();
    if (cfg) {
        cfg->backlight = percent;
        settings_save(cfg);
    }
    if (s_state == POWER_ACTIVE) {
        apply_backlight(s_normal_bl);
    }
}

static void power_task(void *arg)
{
    (void)arg;
    while (1) {
        uint32_t idle = now_ms() - s_last_activity_ms;

        if (s_state == POWER_ACTIVE && idle >= SLEEP_AFTER_MS) {
            enter_state(POWER_SLEEP);
        } else if (s_state == POWER_ACTIVE && idle >= DIM_AFTER_MS) {
            enter_state(POWER_DIM);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void power_mgr_start(void)
{
    if (!s_inited) {
        return;
    }
    xTaskCreate(power_task, "power_task", 3072, NULL, 2, NULL);
}
