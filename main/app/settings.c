#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NS       "bilimouse"
#define NVS_KEY_CFG  "cfg"

/* 全局副本：其它模块通过 settings_get() 读取，避免到处传指针 */
static settings_t s_store;

static void set_defaults(settings_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->next_dx = 0;
    cfg->next_dy = 0;
    cfg->prev_dx = 0;
    cfg->prev_dy = 0;
    cfg->next_calibrated = false;
    cfg->prev_calibrated = false;
    cfg->sensitivity = SENS_MEDIUM;
    cfg->backlight = 80;
    cfg->debounce_ms = 20;
    cfg->long_press_ms = 600;
    cfg->super_long_press_ms = 1500;
    cfg->ultra_long_press_ms = 4000;
    cfg->touch_calibrated = false;
}

const settings_t *settings_get(void)
{
    return &s_store;
}

settings_t *settings_mut(void)
{
    return &s_store;
}

esp_err_t settings_load(settings_t *cfg)
{
    set_defaults(&s_store);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        if (cfg) {
            *cfg = s_store;
        }
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        if (cfg) {
            *cfg = s_store;
        }
        return err;
    }

    /* 结构体变长后旧存档会长度不符，这里按失败处理并回落到默认值 */
    size_t len = sizeof(settings_t);
    settings_t tmp;
    err = nvs_get_blob(h, NVS_KEY_CFG, &tmp, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(settings_t)) {
        s_store = tmp;
        ESP_LOGI(TAG, "config loaded (next_cal=%d prev_cal=%d touch_cal=%d)",
                 s_store.next_calibrated, s_store.prev_calibrated,
                 s_store.touch_calibrated);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no config blob, using defaults");
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "config size changed (%u != %u), using defaults",
                 (unsigned)len, (unsigned)sizeof(settings_t));
    } else {
        ESP_LOGE(TAG, "nvs get failed: %s", esp_err_to_name(err));
    }

    if (cfg) {
        *cfg = s_store;
    }
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *cfg)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_CFG, cfg, sizeof(*cfg));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config saved");
    } else {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(err));
    }
    return err;
}

void settings_reset_default(settings_t *cfg)
{
    set_defaults(&s_store);
    if (cfg) {
        *cfg = s_store;
        settings_save(cfg);
    } else {
        settings_save(&s_store);
    }
}
