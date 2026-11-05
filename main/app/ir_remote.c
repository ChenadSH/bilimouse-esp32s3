/**
 * @file ir_remote.c
 * @brief 红外学习：原始波形学习 -> 双份持久化 -> 原样回放
 *
 * 存档策略（两层，互不依赖）：
 *   1. NVS（namespace "bilimouse", key "irbank"）—— 主副本；
 *   2. 独立 flash 分区 "irbank"（A/B 双扇区 + CRC + 序号）—— 镜像副本。
 *
 *   加载时两份都读，取「序号更大且 CRC 通过」的那份，另一份如果缺失或
 *   过期就顺手写回去。所以：
 *     - 重启、断电       -> 不丢（两份都在 flash 里）
 *     - 普通 idf.py flash -> 不丢（烧写只覆盖 bootloader/分区表/app）
 *     - NVS 被整体擦除   -> 不丢（分区镜像还在，会自动补回 NVS）
 *     - 分区表里 irbank 丢了 -> 不丢（NVS 还在，下次保存时自动重建镜像）
 */
#include "ir_remote.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "bsp_ir.h"
#include "ir_store.h"

static const char *TAG = "ir_remote";

#define NVS_NS          "bilimouse"
#define NVS_KEY_IR      "irbank"
#define NVS_KEY_IR_ACT  "ir_act"

#define IR_BANK_MAGIC    0x49524232u   /* "IRB2" */
#define IR_BANK_V1_MAGIC 0x49524231u   /* "IRB1"：老格式，只用于迁移 */
#define IR_BANK_VERSION  2

#define IR_MIN_SYMS   6             /* 少于 6 个符号基本是噪声 */
#define IR_TRIM_US    20000         /* 超过 20ms 的尾部空闲段剪掉 */

typedef struct {
    uint16_t count;
    uint16_t reserved;
    rmt_symbol_word_t syms[BSP_IR_MAX_SYMBOLS];
} ir_slot_t;

typedef struct {
    uint32_t  magic;
    uint16_t  version;
    uint16_t  slot_count;
    uint32_t  seq;                  /* 每次保存 +1，用来判断哪份更新 */
    ir_slot_t slots[IR_SLOT_COUNT];
} ir_bank_t;

/* ---- 老格式（v2.0 之前）---- */
typedef struct {
    uint16_t count;
    rmt_symbol_word_t syms[BSP_IR_MAX_SYMBOLS];
} ir_slot_v1_t;

typedef struct {
    uint32_t     magic;
    ir_slot_v1_t slots[IR_SLOT_COUNT];
} ir_bank_v1_t;

static ir_bank_t s_bank;
static ir_bank_t s_tmp_a;       /* 放静态区：两份 bank 加起来 3KB，别塞栈 */
static ir_bank_t s_tmp_b;
static uint8_t   s_active;
static bool      s_ready;

/* ==================== NVS 读写 ==================== */

static void bank_reset(ir_bank_t *b)
{
    memset(b, 0, sizeof(*b));
    b->magic      = IR_BANK_MAGIC;
    b->version    = IR_BANK_VERSION;
    b->slot_count = IR_SLOT_COUNT;
    b->seq        = 0;
}

static bool bank_valid(const ir_bank_t *b)
{
    return b->magic == IR_BANK_MAGIC &&
           b->version == IR_BANK_VERSION &&
           b->slot_count == IR_SLOT_COUNT;
}

static esp_err_t nvs_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_IR, &s_bank, sizeof(s_bank));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool nvs_load(ir_bank_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(*out);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_IR, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*out) && bank_valid(out);
}

/** 从 v1 老格式迁移（升级固件后第一次启动跑一次） */
static bool migrate_v1(ir_bank_t *out)
{
    static ir_bank_v1_t v1 __attribute__((aligned(4)));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(v1);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_IR, &v1, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(v1) || v1.magic != IR_BANK_V1_MAGIC) {
        return false;
    }

    bank_reset(out);
    for (int i = 0; i < IR_SLOT_COUNT; i++) {
        uint16_t n = v1.slots[i].count;
        if (n > BSP_IR_MAX_SYMBOLS) {
            n = BSP_IR_MAX_SYMBOLS;
        }
        out->slots[i].count = n;
        if (n) {
            memcpy(out->slots[i].syms, v1.slots[i].syms,
                   n * sizeof(rmt_symbol_word_t));
        }
    }
    ESP_LOGI(TAG, "migrated %u slot(s) from v1 format",
             (unsigned)ir_remote_loaded_count());
    return true;
}

/* ---- 当前槽位：单独存，切换槽位时只写这 1 字节，不重写整个码库 ---- */

static void active_save(uint8_t slot)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, NVS_KEY_IR_ACT, slot);
    nvs_commit(h);
    nvs_close(h);
}

static uint8_t active_load(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    if (nvs_get_u8(h, NVS_KEY_IR_ACT, &v) != ESP_OK || v >= IR_SLOT_COUNT) {
        v = 0;
    }
    nvs_close(h);
    return v;
}

/* ==================== 统一保存 / 载入 ==================== */

/** 写入两份副本。只要有一份成功就算成功（另一份下次会自愈） */
static esp_err_t bank_save(void)
{
    s_bank.seq++;

    esp_err_t e_raw = ir_store_save(&s_bank, sizeof(s_bank), s_bank.seq);
    esp_err_t e_nvs = nvs_save();

    if (e_raw != ESP_OK) {
        ESP_LOGW(TAG, "flash mirror save failed: %s", esp_err_to_name(e_raw));
    }
    if (e_nvs != ESP_OK) {
        ESP_LOGW(TAG, "nvs save failed: %s", esp_err_to_name(e_nvs));
    }

    if (e_raw == ESP_OK || e_nvs == ESP_OK) {
        ESP_LOGI(TAG, "ir bank saved (seq %u, nvs=%s, flash=%s)",
                 (unsigned)s_bank.seq, esp_err_to_name(e_nvs), esp_err_to_name(e_raw));
        return ESP_OK;
    }
    return e_nvs;
}

static esp_err_t bank_load(void)
{
    bank_reset(&s_bank);

    size_t len = 0;
    uint32_t seq_raw = 0;
    bool raw_ok = (ir_store_load(&s_tmp_a, sizeof(s_tmp_a), &len, &seq_raw) == ESP_OK &&
                   len == sizeof(s_tmp_a) && bank_valid(&s_tmp_a));

    bool nvs_ok = nvs_load(&s_tmp_b);

    if (raw_ok && nvs_ok) {
        s_bank = (s_tmp_b.seq > s_tmp_a.seq) ? s_tmp_b : s_tmp_a;
        ESP_LOGI(TAG, "ir bank: flash seq=%u, nvs seq=%u -> using seq=%u",
                 (unsigned)s_tmp_a.seq, (unsigned)s_tmp_b.seq, (unsigned)s_bank.seq);
        /* 两份版本不一致（说明某一次只写成功了一份），把旧的补成新的 */
        if (s_tmp_b.seq != s_tmp_a.seq) {
            if (s_tmp_a.seq != s_bank.seq) {
                ir_store_save(&s_bank, sizeof(s_bank), s_bank.seq);
            }
            if (s_tmp_b.seq != s_bank.seq) {
                nvs_save();
            }
        }
    } else if (raw_ok) {
        s_bank = s_tmp_a;
        ESP_LOGW(TAG, "nvs copy missing - restored %u slot(s) from flash mirror",
                 (unsigned)ir_remote_loaded_count());
        nvs_save();
    } else if (nvs_ok) {
        s_bank = s_tmp_b;
        ESP_LOGW(TAG, "flash mirror missing - rebuilding it from nvs (%u slots)",
                 (unsigned)ir_remote_loaded_count());
        ir_store_save(&s_bank, sizeof(s_bank), s_bank.seq);
    } else if (migrate_v1(&s_bank)) {
        /* 老固件升上来的：把迁移结果落到两份新存档里 */
        ESP_LOGI(TAG, "migrated to v2 storage, writing both copies");
        bank_save();
    } else {
        ESP_LOGI(TAG, "no ir codes stored yet");
        return ESP_OK;
    }

    return ESP_OK;
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
    /* 顺序很重要：先把存档读出来，再初始化硬件。
       上一版硬件初始化失败就直接 return，s_bank 是空的——此时任何一次
       保存（哪怕是清空）都会把已经学好的码覆盖成空库。 */
    ir_store_init();
    bank_load();
    s_active = active_load();

    esp_err_t err = bsp_ir_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ir hw init failed: %s (红外收发不可用，已学码仍保留)",
                 esp_err_to_name(err));
        s_ready = false;
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t ir_remote_learn(uint32_t timeout_ms)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    static rmt_symbol_word_t buf[BSP_IR_MAX_SYMBOLS] __attribute__((aligned(4)));
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
    active_save(s_active);     /* 重启后回到同一个槽，别每次都跳回 0 */
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
    uint32_t keep_seq = s_bank.seq;
    bank_reset(&s_bank);
    s_bank.seq = keep_seq;     /* 保留序号单调性，避免旧副本"复活" */
    s_active = 0;
    active_save(0);
    return bank_save();
}
