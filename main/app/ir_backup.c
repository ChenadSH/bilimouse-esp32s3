/**
 * @file ir_backup.c
 * @brief 红外码库导出/导入的串口实现
 *
 * 一槽一行的设计原因：串口控制台按行收输入，单槽最大 388 字节二进制
 * -> base64 516 字符，任何单行都稳稳落在控制台行长上限之内，
 * 「整文件粘贴」和「逐行粘贴」都能工作。
 */
#include "ir_backup.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"

#include "bsp_ir.h"
#include "ir_remote.h"

/* base64 用 mbedtls 现成实现（bt 组件已把 mbedtls 带进工程） */
#include "mbedtls/base64.h"

#define B64_CAP  800   /* 388 字节 -> 516 字符，留足余量 */

/** rmt_symbol_word_t 的原始 4 字节布局（小端）：
 *  duration0:16 level0:1... duration1:16 level1:1 —— 原样搬运即可 */
static int emit_slot(int slot, char *b64buf, size_t b64cap)
{
    rmt_symbol_word_t syms[BSP_IR_MAX_SYMBOLS];
    uint16_t count = 0;
    esp_err_t err = ir_remote_read_slot((uint8_t)slot, syms, &count);
    if (err != ESP_OK) {
        printf("BMIR1 slot=%d state=empty\n", slot);
        return 0;
    }

    uint8_t bin[4 + BSP_IR_MAX_SYMBOLS * 4];
    bin[0] = (uint8_t)(count & 0xff);
    bin[1] = (uint8_t)(count >> 8);
    memcpy(bin + 2, syms, count * sizeof(rmt_symbol_word_t));

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64buf, b64cap, &olen,
                              bin, 2 + count * 4u) != 0) {
        printf("# error: slot %d base64 encode failed\n", slot);
        return 1;
    }
    b64buf[olen] = '\0';
    printf("BMIR1 slot=%d syms=%u b64=%s\n",
           slot, (unsigned)count, b64buf);
    return 0;
}

static int cmd_ir_list(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    printf("ir bank seq=%u loaded=%u/%u\n",
           (unsigned)ir_remote_bank_seq(),
           (unsigned)ir_remote_loaded_count(), (unsigned)IR_SLOT_COUNT);
    for (int i = 0; i < IR_SLOT_COUNT; i++) {
        printf("slot %2d: %s\n", i,
               ir_remote_slot_loaded((uint8_t)i) ? "loaded" : "empty");
    }
    return 0;
}

static int cmd_ir_export(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: ir export <all|slot 0-%d>\n", IR_SLOT_COUNT - 1);
        return 1;
    }

    static char b64buf[B64_CAP];

    if (strcmp(argv[1], "all") == 0) {
        printf("# BiliMouse IR backup, seq=%u. Restore one line at a time:\n",
               (unsigned)ir_remote_bank_seq());
        printf("#   ir import <slot> <b64>\n");
        for (int i = 0; i < IR_SLOT_COUNT; i++) {
            emit_slot(i, b64buf, B64_CAP);
        }
        return 0;
    }

    int slot = atoi(argv[1]);
    if (slot < 0 || slot >= IR_SLOT_COUNT) {
        printf("error: slot out of range 0..%d\n", IR_SLOT_COUNT - 1);
        return 1;
    }
    return emit_slot(slot, b64buf, B64_CAP);
}

static int cmd_ir_import(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: ir import <slot 0-%d> <b64>\n", IR_SLOT_COUNT - 1);
        printf("       (b64 comes from 'ir export'; paste the whole line tail)\n");
        return 1;
    }
    int slot = atoi(argv[1]);
    if (slot < 0 || slot >= IR_SLOT_COUNT) {
        printf("error: slot out of range 0..%d\n", IR_SLOT_COUNT - 1);
        return 1;
    }

    uint8_t bin[4 + BSP_IR_MAX_SYMBOLS * 4];
    size_t olen = 0;
    if (mbedtls_base64_decode(bin, sizeof(bin), &olen,
                              (const unsigned char *)argv[2],
                              strlen(argv[2])) != 0) {
        printf("error: not valid base64\n");
        return 1;
    }
    if (olen < 2) {
        printf("error: payload too short\n");
        return 1;
    }
    uint16_t count = (uint16_t)(bin[0] | (bin[1] << 8));
    if (count == 0 || count > BSP_IR_MAX_SYMBOLS ||
        olen != 2u + count * 4u) {
        printf("error: bad payload (count=%u, len=%u)\n",
               (unsigned)count, (unsigned)olen);
        return 1;
    }

    esp_err_t err = ir_remote_write_slot((uint8_t)slot, bin + 2, count);
    if (err != ESP_OK) {
        printf("error: import failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("ok: slot %d <- %u symbols (saved to nvs + irbank)\n",
           slot, (unsigned)count);
    return 0;
}

static int cmd_ir_clear(int argc, char **argv)
{
    if (argc != 2 || strcmp(argv[1], "confirm") != 0) {
        printf("this ERASES all learned IR codes. run: ir clear confirm\n");
        return 1;
    }
    esp_err_t err = ir_remote_clear_all();
    printf(err == ESP_OK ? "ok: all slots cleared\n"
                         : "error: %s\n", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

esp_err_t ir_backup_register(void)
{
    /* 子命令拆成独立命令注册：比在回调里再解析一层更省事也更稳 */
    esp_console_cmd_t list_cmd = {
        .command = "ir_list", .help = "List IR slots and bank status",
        .func = cmd_ir_list,
    };
    esp_console_cmd_t export_cmd = {
        .command = "ir_export",
        .help = "Export IR slot(s) as paste-able base64 lines",
        .hint = "<all|slot>",
        .func = cmd_ir_export,
    };
    esp_console_cmd_t import_cmd = {
        .command = "ir_import",
        .help = "Import one slot from 'ir export' base64 line",
        .hint = "<slot> <b64>",
        .func = cmd_ir_import,
    };
    esp_console_cmd_t clear_cmd = {
        .command = "ir_clear",
        .help = "Erase ALL learned IR codes",
        .hint = "confirm",
        .func = cmd_ir_clear,
    };
    esp_err_t err;
    err = esp_console_cmd_register(&list_cmd);   if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&export_cmd); if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&import_cmd); if (err != ESP_OK) return err;
    err = esp_console_cmd_register(&clear_cmd);  if (err != ESP_OK) return err;
    return ESP_OK;
}
