#include "ir_remote.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "bsp_ir.h"

static const char *TAG = "ir_remote";

#define NVS_NS        "bilimouse"
#define NVS_KEY_IR    "irbank"
#define IR_MAGIC      0x49524231u   /* "IRB1"，格式版本，改了要重新学 */
#define IR_MIN_SYMS   6             /* 少于 6 个符号基本是噪声 */
#define IR_TRIM_US    20000         /* 超过 20ms 的尾部空闲段剪掉 */

/* NVS 里存的一整块：magic + 4 个槽 */
typedef struct {
    uint16_t count;
    rmt_symbol_word_t syms[BSP_IR_MAX_SYMBOLS];
} ir_slot_t;

typedef struct {
    uint32_t  magic;
    ir_slot_t slots[IR_SLOT_COUNT];
} ir_bank_t;

static ir_bank_t s_bank;
static uint8_t   s_active;
static bool      s_ready;

/* ==================== NVS ==================== */

static esp_err_t bank_load(void)
{
    memset(&s_bank, 0, sizeof(s_bank));
    s_bank.magic = IR_MAGIC;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return ESP_OK;   /* 没存过，用空库 */
    }
    ir_bank_t tmp;
    size_t len = sizeof(tmp);
    err = nvs_get_blob(h, NVS_KEY_IR, &tmp, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(tmp) && tmp.magic == IR_MAGIC) {
        s_bank = tmp;
        ESP_LOGI(TAG, "ir bank loaded (%u slots)", (unsigned)ir_remote_loaded_count());
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "ir bank format mismatch, starting empty");
    }
    return ESP_OK;
}

static esp_err_t bank_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_IR, &s_bank, sizeof(s_bank));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/* ==================== 波形处理 ==================== */

/**
 * 剪掉尾部过长的空闲段。
 *
 * 接收时我们靠「静默 40ms」判定一帧结束，这 40ms 会被记成最后一段
 * level=0 的长时间电平。原样回放虽然无害，但它会占掉一个 symbol 名额，
 * 而且显得很脏。顺便去掉时长为 0 的空洞符号。
 */
static size_t trim_symbols(rmt_symbol_word_t *syms, size_t n)
{
    while (n > 0) {
        rmt_symbol_word_t *last = &syms[n - 1];
        bool tail_idle = (last->level0 == 0 && last->duration0 > IR_TRIM_US) ||
                         (last->level1 == 0 && last->duration1 > IR_TRIM_US);
        bool empty = (last->duration0 == 0 && last->duration1 == 0);
        if (!tail_idle && !empty) {
            break;
        }
        n--;
    }
    return n;
}

/* ==================== 对外接口 ==================== */

bool ir_remote_ready(void)
{
    return s_ready;
}

esp_err_t ir_remote_init(void)
{
    esp_err_t err = bsp_ir_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ir hw init failed: %s (红外功能不可用)", esp_err_to_name(err));
        s_ready = false;
        return err;
    }
    s_ready = true;
    bank_load();
    return ESP_OK;
}

esp_err_t ir_remote_learn(uint32_t timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    rmt_symbol_word_t buf[BSP_IR_MAX_SYMBOLS];
    ESP_LOGI(TAG, "learning slot %u, waiting for signal...", s_active);
    size_t n = bsp_ir_capture(buf, BSP_IR_MAX_SYMBOLS, timeout_ms);
    if (n < IR_MIN_SYMS) {
        ESP_LOGW(TAG, "no valid signal captured (%u syms)", (unsigned)n);
        return ESP_ERR_TIMEOUT;
    }

    n = trim_symbols(buf, n);
    if (n < IR_MIN_SYMS) {
        ESP_LOGW(TAG, "signal too short after trim (%u syms)", (unsigned)n);
        return ESP_ERR_TIMEOUT;
    }

    ir_slot_t *slot = &s_bank.slots[s_active];
    memset(slot, 0, sizeof(*slot));
    slot->count = (uint16_t)n;
    memcpy(slot->syms, buf, n * sizeof(rmt_symbol_word_t));

    ESP_LOGI(TAG, "slot %u learned: %u symbols, first=%u/%u us",
             s_active, (unsigned)n, buf[0].duration0, buf[0].duration1);
    return bank_save();
}

esp_err_t ir_remote_send(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    ir_slot_t *slot = &s_bank.slots[s_active];
    if (slot->count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "replay slot %u (%u syms)", s_active, slot->count);
    return bsp_ir_send_raw(slot->syms, slot->count);
}

bool ir_remote_slot_loaded(uint8_t slot)
{
    return slot < IR_SLOT_COUNT && s_bank.slots[slot].count > 0;
}

uint8_t ir_remote_active_slot(void)
{
    return s_active;
}

void ir_remote_next_slot(void)
{
    s_active = (uint8_t)((s_active + 1) % IR_SLOT_COUNT);
}

uint8_t ir_remote_loaded_count(void)
{
    uint8_t n = 0;
    for (int i = 0; i < IR_SLOT_COUNT; i++) {
        if (s_bank.slots[i].count > 0) {
            n++;
        }
    }
    return n;
}

esp_err_t ir_remote_clear_all(void)
{
    memset(&s_bank, 0, sizeof(s_bank));
    s_bank.magic = IR_MAGIC;
    s_active = 0;
    return bank_save();
}
