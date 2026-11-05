/**
 * @file cli.c
 * @brief 串口命令行启动
 *
 * 不用 IDF 默认的 CONFIG_ESP_CONSOLE_CONSOLE组件自动挂载，而是显式
 * 建 UART REPL：行缓冲给大（导出的一行 base64 有 500+ 字符），
 * 历史记录关闭（不占 SPIFFS/SD，也不需要在掉电时持久化）。
 */
#include "cli.h"

#include "esp_console.h"
#include "esp_log.h"

#include "driver/uart_vfs.h"

#include "ir_backup.h"
#include "ota_update.h"
#include "radio.h"

static const char *TAG = "cli";

static esp_console_repl_t *s_repl;

esp_err_t cli_start(void)
{
    if (s_repl) {
        return ESP_OK;
    }

    /* 先注册命令，再启动 REPL（顺序其实无所谓，但这样日志更整洁） */
    esp_err_t err = ir_backup_register();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ir_backup register failed: %s", esp_err_to_name(err));
    }
    radio_register_cli();
    /* ota_update_init 里会注册 wifi/ota 命令（app_main 已调用） */

    esp_console_dev_uart_config_t uart_cfg = {
        .channel = 0,
        .baud_rate = 115200,
        .rx_gpio_num = -1,   /* -1 = 用 CONFIG 默认引脚 */
        .tx_gpio_num = -1,
    };
    esp_console_repl_config_t repl_cfg = {
        /* linenoise 拒绝 len<1：max_history_len=0 会让 REPL 创建中途失败，
           而 IDF 的清理路径只删 UART 驱动不切回 VFS nonblocking 模式，
           之后所有日志写入都报错并递归打日志 -> 栈爆 -> 重启循环。
           所以这里用 1（等效"无历史"），绝不能用 0。 */
        .max_history_len = 1,
        .prompt = "bilimouse> ",
        .max_cmdline_length = 1024,    /* 单槽导出行 ~516 字符 */
    };

    err = esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &s_repl);
    if (err != ESP_OK) {
        /* 防护：万一创建失败，把 VFS 切回 nonblocking，否则驱动已删
           而日志还往驱动写，会在 uart_write_bytes 里无限递归崩溃 */
        uart_vfs_dev_use_nonblocking(0);
        ESP_LOGW(TAG, "repl create failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_console_start_repl(s_repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "repl start failed: %s", esp_err_to_name(err));
        s_repl = NULL;
        return err;
    }
    ESP_LOGI(TAG, "serial console ready (ir_export / wifi / ota)");
    return ESP_OK;
}
