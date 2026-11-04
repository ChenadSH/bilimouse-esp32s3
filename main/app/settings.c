#include "settings.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NS       "bilimouse"
#define NVS_KEY_CFG  "cfg"

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
}

esp_err_t settings_load(settings_t *cfg)
{
    set_defaults(cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no saved config, using defaults");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len = sizeof(*cfg);
    err = nvs_get_blob(h, NVS_KEY_CFG, cfg, &len);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "config loaded (next_cal=%d prev_cal=%d sens=%d)",
                 cfg->next_calibrated, cfg->prev_calibrated, cfg->sensitivity);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "no config blob, using defaults");
        set_defaults(cfg);
    } else {
        ESP_LOGE(TAG, "nvs get failed: %s", esp_err_to_name(err));
        set_defaults(cfg);
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
    set_defaults(cfg);
    settings_save(cfg);
}
