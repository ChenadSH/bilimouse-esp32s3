#include "ir_store.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"

static const char *TAG = "ir_store";

#define IR_PART_LABEL   "irbank"
#define SECTOR_SZ       4096
#define REC_HDR_SZ      16
#define REC_MAGIC       0x49525354u   /* "IRST" */

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint32_t len;
    uint32_t crc;
} rec_hdr_t;

static const esp_partition_t *s_part;

/* 读写用的对齐暂存区。esp_partition 读写本身允许 PSRAM 源（内部走 memcpy），
   放 PSRAM 省下 4KB 内部 RAM 给 WiFi/LCD DMA。 */
static uint8_t s_io[IR_STORE_MAX_REC] EXT_RAM_BSS_ATTR __attribute__((aligned(4)));

static inline uint32_t rec_crc(const void *buf, size_t len)
{
    return esp_rom_crc32_le(0, (const uint8_t *)buf, (uint32_t)len);
}

static inline uint32_t sector_addr(int idx)
{
    return (uint32_t)idx * SECTOR_SZ;
}

/**
 * 读一个扇区上的记录。
 * @param buf 为 NULL 时只做校验，不拷贝数据
 */
static bool read_rec(int idx, void *buf, size_t cap, size_t *out_len, uint32_t *out_seq)
{
    if (!s_part) {
        return false;
    }

    rec_hdr_t h;
    if (esp_partition_read(s_part, sector_addr(idx), &h, sizeof(h)) != ESP_OK) {
        return false;
    }
    if (h.magic != REC_MAGIC || h.len == 0 || h.len > IR_STORE_MAX_REC) {
        return false;
    }
    if (h.len > cap) {
        return false;
    }
    if (esp_partition_read(s_part, sector_addr(idx) + REC_HDR_SZ, s_io, h.len) != ESP_OK) {
        return false;
    }
    if (rec_crc(s_io, h.len) != h.crc) {
        ESP_LOGW(TAG, "sector %d crc mismatch, ignored", idx);
        return false;
    }

    if (buf) {
        memcpy(buf, s_io, h.len);
    }
    if (out_len) {
        *out_len = h.len;
    }
    if (out_seq) {
        *out_seq = h.seq;
    }
    return true;
}

esp_err_t ir_store_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                      (esp_partition_subtype_t)0x40,
                                      IR_PART_LABEL);
    if (!s_part) {
        ESP_LOGW(TAG, "partition '%s' not found - IR codes will live in NVS only",
                 IR_PART_LABEL);
        return ESP_ERR_NOT_FOUND;
    }
    if (s_part->size < 2 * SECTOR_SZ) {
        ESP_LOGW(TAG, "partition '%s' too small (%u bytes)", IR_PART_LABEL,
                 (unsigned)s_part->size);
        s_part = NULL;
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "ir mirror at 0x%08x, %u KB", (unsigned)s_part->address,
             (unsigned)(s_part->size / 1024));
    return ESP_OK;
}

bool ir_store_available(void)
{
    return s_part != NULL;
}

esp_err_t ir_store_load(void *buf, size_t cap, size_t *out_len, uint32_t *out_seq)
{
    size_t len_a = 0, len_b = 0;
    uint32_t seq_a = 0, seq_b = 0;

    bool ok_a = read_rec(0, NULL, cap, &len_a, &seq_a);
    bool ok_b = read_rec(1, NULL, cap, &len_b, &seq_b);

    if (ok_a && ok_b) {
        return read_rec(seq_a >= seq_b ? 0 : 1, buf, cap, out_len, out_seq)
                   ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    if (ok_a) {
        return read_rec(0, buf, cap, out_len, out_seq) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    if (ok_b) {
        return read_rec(1, buf, cap, out_len, out_seq) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t ir_store_save(const void *buf, size_t len, uint32_t seq)
{
    if (!s_part) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len == 0 || len > IR_STORE_MAX_REC || len > SECTOR_SZ - REC_HDR_SZ) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* 决定牺牲哪一页：两个都有效就覆盖序号小的那个，只有一个有效就写空的那个 */
    uint32_t seq_a = 0, seq_b = 0;
    bool ok_a = read_rec(0, NULL, IR_STORE_MAX_REC, NULL, &seq_a);
    bool ok_b = read_rec(1, NULL, IR_STORE_MAX_REC, NULL, &seq_b);

    int target;
    if (ok_a && ok_b) {
        target = (seq_a <= seq_b) ? 0 : 1;
    } else if (ok_a) {
        target = 1;
    } else {
        target = 0;
    }

    /* 先擦后写：只有目标页会没，另一页始终是完整的旧副本 */
    esp_err_t err = esp_partition_erase_range(s_part, sector_addr(target), SECTOR_SZ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase sector %d failed: %s", target, esp_err_to_name(err));
        return err;
    }

    rec_hdr_t h = {
        .magic = REC_MAGIC,
        .seq   = seq,
        .len   = (uint32_t)len,
        .crc   = rec_crc(buf, len),
    };

    memcpy(s_io, buf, len);
    size_t padded = (len + 3u) & ~(size_t)3u;      /* flash 写按 4 字节对齐 */
    if (padded > len) {
        memset(s_io + len, 0, padded - len);
    }

    err = esp_partition_write(s_part, sector_addr(target), &h, sizeof(h));
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, sector_addr(target) + REC_HDR_SZ, s_io, padded);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write sector %d failed: %s", target, esp_err_to_name(err));
        return err;
    }

    /* 回读校验：写完立刻验一遍，有问题就当没写成（旧副本还在） */
    size_t vlen = 0;
    uint32_t vseq = 0;
    if (!read_rec(target, NULL, IR_STORE_MAX_REC, &vlen, &vseq) || vlen != len || vseq != seq) {
        ESP_LOGE(TAG, "verify sector %d failed", target);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "saved %u bytes to sector %d (seq %u)",
             (unsigned)len, target, (unsigned)seq);
    return ESP_OK;
}
