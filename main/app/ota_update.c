/**
 * @file ota_update.c
 * @brief WiFi STA + manifest 检查 + esp_https_ota 流式升级
 *
 * WiFi 凭据优先级：NVS（串口 `wifi set`）> Kconfig 编译期默认。
 * manifest 地址同理：NVS（`ota url`）> Kconfig BILIMOUSE_OTA_URL。
 */
#include "ota_update.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "power_mgr.h"
#include "sdkconfig.h"
#include "ui.h"

static const char *TAG = "ota";

#define NVS_NS          "bilimouse"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_OTA_URL "ota_url"

#define WIFI_TIMEOUT_MS   9000
#define OTA_TASK_STACK    9216
#define AUTO_CHECK_DELAY_S 30

/* ==================== WiFi ==================== */

static bool s_wifi_inited;
static volatile bool s_wifi_got_ip;
static esp_event_handler_instance_t s_ev_any;
static esp_event_handler_instance_t s_ev_ip;

/* 升级任务防重入（定义在文件后面的 ota_update_start_manual 里用） */
static volatile bool s_ota_running;

/* 进度更新（实现见「升级执行」小节），WiFi 阶段就要用来刷屏 */
static void prog_set(const char *stage, const char *detail);

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_got_ip = false;
        ESP_LOGW(TAG, "wifi disconnected, retrying...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        s_wifi_got_ip = true;
        ESP_LOGI(TAG, "wifi got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return err;
    }
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        wifi_event_cb, NULL, &s_ev_any);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        wifi_event_cb, NULL, &s_ev_ip);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_wifi_inited = true;
    return ESP_OK;
}

static esp_err_t wifi_init_once(void);
static esp_err_t wifi_ensure_connected(void);

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;

/** 候选凭据：NVS（串口设置）> 出厂默认1 > 出厂默认2（备用）。
 *  返回数量，nvs_used 标记第一个是否来自 NVS。 */
static int creds_collect(wifi_cred_t *list, int max, bool *nvs_used)
{
    int n = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l1 = sizeof(list[0].ssid), l2 = sizeof(list[0].pass);
        if (nvs_get_str(h, NVS_KEY_SSID, list[0].ssid, &l1) == ESP_OK &&
            nvs_get_str(h, NVS_KEY_PASS, list[0].pass, &l2) == ESP_OK &&
            list[0].ssid[0] != '\0') {
            n = 1;
        }
        nvs_close(h);
    }
    *nvs_used = n > 0;
    if (n < max) {
        strlcpy(list[n].ssid, CONFIG_BILIMOUSE_WIFI_SSID, sizeof(list[n].ssid));
        strlcpy(list[n].pass, CONFIG_BILIMOUSE_WIFI_PASS, sizeof(list[n].pass));
        if (list[n].ssid[0] != '\0') {
            n++;
        }
    }
    if (n < max) {
        strlcpy(list[n].ssid, CONFIG_BILIMOUSE_WIFI_SSID2, sizeof(list[n].ssid));
        strlcpy(list[n].pass, CONFIG_BILIMOUSE_WIFI_PASS2, sizeof(list[n].pass));
        if (list[n].ssid[0] != '\0') {
            n++;
        }
    }
    if (n < max) {
        strlcpy(list[n].ssid, CONFIG_BILIMOUSE_WIFI_SSID3, sizeof(list[n].ssid));
        strlcpy(list[n].pass, CONFIG_BILIMOUSE_WIFI_PASS3, sizeof(list[n].pass));
        if (list[n].ssid[0] != '\0') {
            n++;
        }
    }
    return n;
}

/** 对单个凭据尝试连接。成功返回 true 并保持连接。 */
static bool wifi_try_cred(const wifi_cred_t *c)
{
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->pass, sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();

    for (int waited = 0; waited < WIFI_TIMEOUT_MS && !s_wifi_got_ip; waited += 200) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    return s_wifi_got_ip;
}

bool ota_update_wifi_connected(void)
{
    return s_wifi_got_ip;
}

esp_err_t ota_update_wifi_online(void)
{
    return wifi_ensure_connected();
}

static esp_err_t wifi_ensure_connected(void)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        return err;
    }
    if (s_wifi_got_ip) {
        return ESP_OK;
    }

    wifi_cred_t list[3];
    bool nvs_used = false;
    int n = creds_collect(list, 3, &nvs_used);
    if (n == 0) {
        ESP_LOGW(TAG, "no wifi credentials (serial: 'wifi set <ssid> <pass>')");
        return ESP_ERR_INVALID_STATE;
    }

    bool any_configured = false;
    for (int i = 0; i < n && !s_wifi_got_ip; i++) {
        ESP_LOGI(TAG, "wifi connecting: %s%s", list[i].ssid,
                 (i == 0 && nvs_used) ? " (NVS)" : "");
        any_configured = true;
        /* 屏上同步显示正在试哪个热点：之前三个热点串行各等 20s，
           屏幕毫无变化，用户以为卡死了。现在每换一个热点都有反馈，
           且单个热点最多等 9s，最坏 27s 内出结论。 */
        prog_set("连接WiFi", list[i].ssid);
        ui_show_action("连接 WiFi", list[i].ssid);
        wifi_try_cred(&list[i]);
        if (s_wifi_got_ip) {
            ESP_LOGI(TAG, "wifi connected via %s", list[i].ssid);
        }
    }
    (void)any_configured;
    return s_wifi_got_ip ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ==================== manifest ==================== */

typedef struct {
    char version[24];
    char url[192];
    char notes[96];
    int  size;       /* 升级包字节数（可选字段，旧 manifest 没有 = 0） */
} ota_manifest_t;

/** manifest URL：NVS 覆盖 Kconfig */
static bool manifest_url_get(char *buf, size_t len)
{
    nvs_handle_t h;
    bool got = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t l = len;
        got = (nvs_get_str(h, NVS_KEY_OTA_URL, buf, &l) == ESP_OK);
        nvs_close(h);
    }
    if (!got) {
        strlcpy(buf, CONFIG_BILIMOUSE_OTA_URL, len);
    }
    return buf[0] != '\0';
}

/** 解析 "MAJOR.MINOR.PATCH"，失败返回 -1 */
static long semver_field(const char **p)
{
    char *end;
    long v = strtol(*p, &end, 10);
    *p = end;
    return v;
}

/** >0: a 更新; <0: b 更新; 0: 相同。容忍 2 段/3 段版本号 */
static int semver_cmp(const char *a, const char *b)
{
    const char *pa = a, *pb = b;
    for (int i = 0; i < 3; i++) {
        long va = semver_field(&pa);
        long vb = semver_field(&pb);
        if (va != vb) {
            return (va > vb) ? 1 : -1;
        }
        if (*pa == '.') pa++;
        if (*pb == '.') pb++;
        if (!*pa && !*pb) {
            return 0;
        }
    }
    return 0;
}

/** HTTP GET manifest 并解析 JSON。失败返回错误码。 */
static esp_err_t manifest_fetch(ota_manifest_t *m)
{
    char url[256];
    if (!manifest_url_get(url, sizeof(url))) {
        ESP_LOGW(TAG, "manifest url empty (serial: 'ota url <http://...>')");
        return ESP_ERR_INVALID_STATE;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        /* GitHub / gitee / 自建 HTTPS 都能过校验：证书包内置主流 CA 根证书。
           局域网 http:// 地址不受影响（bundle 只对 https 生效）。 */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(cli, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "manifest open failed (%s): %s", esp_err_to_name(err), url);
        esp_http_client_cleanup(cli);
        return err;
    }
    int status = esp_http_client_fetch_headers(cli);
    (void)status;
    int code = esp_http_client_get_status_code(cli);

    char body[1024] = {0};
    int n = 0, total = 0;
    while (total < (int)sizeof(body) - 1 &&
           (n = esp_http_client_read(cli, body + total, sizeof(body) - 1 - total)) > 0) {
        total += n;
    }
    esp_http_client_cleanup(cli);

    if (code != 200 || total <= 0) {
        ESP_LOGW(TAG, "manifest http %d, %d bytes (%s)", code, total, url);
        return ESP_ERR_NOT_FOUND;
    }
    body[total] = '\0';

    /* 只提取三个字符串字段，手写扫描比引 cJSON 少一层依赖：
       manifest 是我们自己发布的，格式固定，没必要上完整 JSON 解析器。 */
    memset(m, 0, sizeof(*m));
    const char *v = strstr(body, "\"version\"");
    const char *u = strstr(body, "\"url\"");
    if (v) {
        v = strchr(v + 9, '"');
        if (v) strlcpy(m->version, v + 1, sizeof(m->version));
        char *q = strchr(m->version, '"');
        if (q) *q = '\0';
    }
    if (u) {
        u = strchr(u + 5, '"');
        if (u) strlcpy(m->url, u + 1, sizeof(m->url));
        char *q = strchr(m->url, '"');
        if (q) *q = '\0';
    }
    const char *s = strstr(body, "\"size\"");
    if (s) {
        m->size = atoi(strchr(s + 6, ':') + 1);
    }
    if (m->version[0] == '\0' || m->url[0] == '\0') {
        ESP_LOGW(TAG, "manifest missing version/url");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "manifest: version=%s url=%s", m->version, m->url);
    return ESP_OK;
}

/* ==================== 升级执行 ==================== */

/* 升级进度（升级页面显示用）：ota 任务写，UI 读。
   各字段都是对齐字，无需加锁。 */
static ota_progress_t s_prog;

void ota_update_get_progress(ota_progress_t *out)
{
    if (out) {
        *out = s_prog;
    }
}

static void prog_set(const char *stage, const char *detail)
{
    strlcpy(s_prog.stage, stage, sizeof(s_prog.stage));
    if (detail) {
        strlcpy(s_prog.detail, detail, sizeof(s_prog.detail));
    }
    ui_mark_dirty();
}

/** 下载 + 写 flash + 切启动分区。成功不返回（重启）。 */
static esp_err_t ota_perform(const ota_manifest_t *m)
{
    esp_http_client_config_t http_cfg = {
        .url = m->url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 同 manifest：支持 GitHub HTTPS */
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t h = NULL;
    prog_set("下载中", "连接升级服务器…");
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        prog_set("失败", "连接升级服务器失败");
        return err;
    }
    /* manifest 没带 size 时，这里能拿到镜像真实大小 */
    int img_total = esp_https_ota_get_image_size(h);
    if (img_total > 0) {
        s_prog.total_kb = img_total / 1024;
    }

    /* 读到镜像头后先看一眼新固件的版本，再决定要不要继续 */
    esp_app_desc_t desc;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (esp_https_ota_get_image_len_read(h) >= (int)sizeof(esp_image_header_t) + (int)sizeof(esp_image_segment_header_t) &&
            esp_https_ota_get_img_desc(h, &desc) == ESP_OK) {
            ESP_LOGI(TAG, "remote image version: %s", desc.version);
            prog_set("下载中", desc.version);
        }
        int total = esp_https_ota_get_image_size(h);
        int read = esp_https_ota_get_image_len_read(h);
        s_prog.total_kb = total / 1024;
        s_prog.done_kb = read / 1024;
        if (total > 0) {
            s_prog.percent = read * 100 / total;
            char sub[64];
            snprintf(sub, sizeof(sub), "下载 %d%%", s_prog.percent);
            ui_show_action("升级中", sub);
            power_mgr_notify_activity();   /* 别让屏幕在升级途中睡着 */
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        prog_set("失败", "下载中断，旧固件继续运行");
        return err;
    }

    err = esp_https_ota_finish(h);   /* 内部已做镜像校验 + set_boot_partition */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota finish failed: %s", esp_err_to_name(err));
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            ESP_LOGE(TAG, "image validation failed - keeping old firmware");
            prog_set("失败", "固件校验失败，旧固件继续运行");
        }
        return err;
    }
    ESP_LOGI(TAG, "ota complete, boot partition switched");
    s_prog.percent = 100;
    prog_set("完成", "校验通过，准备重启");
    return ESP_OK;
}

/** 升级任务的主体：连网 -> 比版本 -> 下载 -> 倒计时重启。
 *  自动模式下失败一律静默（不能每 30 秒骚扰一次屏幕），
 *  手动模式下失败给出屏幕提示 + 串口原因。 */
static void ota_run(bool auto_triggered)
{
    power_mgr_notify_activity();

    /* ---- 1. 确保 WiFi 在线 ---- */
    memset(&s_prog, 0, sizeof(s_prog));
    if (!ota_update_wifi_connected()) {
        prog_set("连接WiFi", "正在连接…");
        if (!auto_triggered) {
            ui_show_action("检查更新", "连接 WiFi 中");
        }
        esp_err_t e = wifi_ensure_connected();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect failed: %s", esp_err_to_name(e));
            prog_set("失败", "WiFi 连接失败");
            if (!auto_triggered) {
                ui_show_action("检查更新失败",
                               e == ESP_ERR_INVALID_STATE ? "未配置 WiFi" : "连接超时");
                ui_set_footer("串口: wifi set <ssid> <pass>");
            }
            return;
        }
    }

    /* ---- 2. 拉取版本清单 ---- */
    prog_set("检查清单", "获取版本信息…");
    ota_manifest_t m;
    esp_err_t e = manifest_fetch(&m);
    if (e != ESP_OK) {
        prog_set("失败", "无法获取清单");
        if (!auto_triggered) {
            ui_show_action("检查更新失败", "无法获取清单");
            ui_set_footer("串口: ota url <manifest 地址>");
        }
        return;
    }

    /* ---- 3. 版本比较 ---- */
    const esp_app_desc_t *cur = esp_app_get_description();
    int cmp = semver_cmp(m.version, cur->version);
    if (cmp <= 0) {
        ESP_LOGI(TAG, "firmware up to date (%s)", cur->version);
        prog_set("已是最新", cur->version);
        if (!auto_triggered) {
            char sub[64];
            snprintf(sub, sizeof(sub), "当前 %s 已是最新", cur->version);
            ui_show_action("已是最新", sub);
        }
        return;
    }

    char sub[64];
    snprintf(sub, sizeof(sub), "%s -> %s", cur->version, m.version);
    ui_show_action("发现新版本", sub);
    ESP_LOGI(TAG, "new version available: %s -> %s", cur->version, m.version);
    power_mgr_notify_activity();

    /* manifest 里带了包大小就先亮出来，不用等下载开始才知道 */
    if (m.size > 0) {
        s_prog.total_kb = m.size / 1024;
    }

    /* ---- 4. 下载写入备槽 ---- */
    if (ota_perform(&m) != ESP_OK) {
        if (strcmp(s_prog.stage, "失败") != 0) {
            prog_set("失败", "旧固件继续运行");
        }
        ui_show_action("升级失败", "旧固件继续运行");
        ui_set_footer("原因见串口日志");
        return;
    }

    /* ---- 5. 倒计时重启进新固件。
            新固件启动在 PENDING_VERIFY 状态，app_main 走完才确认；
            若新固件起不来，bootloader 自动回滚旧版本。 ---- */
    ui_show_action("升级完成", "5 秒后重启");
    for (int i = 5; i > 0; i--) {
        char s[24];
        snprintf(s, sizeof(s), "%d 秒后重启", i);
        ui_show_hint(s);
        power_mgr_notify_activity();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_restart();
}

static void ota_manual_task(void *arg)
{
    (void)arg;
    ota_run(false);
    s_ota_running = false;
    vTaskDelete(NULL);
}

void ota_update_start_manual(void)
{
    /* 防重入：连点「开始检查更新」会起多个任务同时抢 WiFi/flash，
     * 轻则进度条乱跳，重则两个 esp_https_ota 写坏分区表。 */
    if (s_ota_running) {
        ESP_LOGW(TAG, "ota already running, ignore");
        ui_show_hint("检查更新进行中");
        return;
    }
    s_ota_running = true;
    if (xTaskCreate(ota_manual_task, "ota_task", OTA_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_ota_running = false;
    }
}

/* ==================== 自动检查 ==================== */

#if CONFIG_BILIMOUSE_OTA_AUTO_CHECK
static void auto_check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(AUTO_CHECK_DELAY_S * 1000));
    /* 自动流程不弹错误提示（WiFi 没配好就静默跳过），只在
       确认发现新版本时才打扰用户。 */
    ota_run(true);
    vTaskDelete(NULL);
}
#endif

/* ==================== 串口命令 ==================== */

#include "esp_console.h"

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        wifi_cred_t list[3];
        bool nvs_used = false;
        creds_collect(list, 3, &nvs_used);
        printf("wifi %s, configured ssid: %s%s\n",
               s_wifi_got_ip ? "connected" : "not connected", list[0].ssid,
               nvs_used ? " (NVS)" : "");
        return 0;
    }
    if (argc == 4 && strcmp(argv[1], "set") == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            printf("error: nvs not ready\n");
            return 1;
        }
        nvs_set_str(h, NVS_KEY_SSID, argv[2]);
        nvs_set_str(h, NVS_KEY_PASS, argv[3]);
        nvs_commit(h);
        nvs_close(h);
        printf("ok: credentials saved, connecting...\n");
        s_wifi_got_ip = false;
        wifi_ensure_connected();
        printf("wifi %s\n", s_wifi_got_ip ? "connected" : "FAILED (check ssid/pass)");
        return 0;
    }
    printf("usage: wifi | wifi set <ssid> <pass>\n");
    return 1;
}

static int cmd_ota(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "url") == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            printf("error: nvs not ready\n");
            return 1;
        }
        nvs_set_str(h, NVS_KEY_OTA_URL, argv[2]);
        nvs_commit(h);
        nvs_close(h);
        printf("ok: manifest url saved\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "check") == 0) {
        ota_update_start_manual();
        printf("check started, watch the screen / serial log\n");
        return 0;
    }
    printf("usage: ota url <manifest-url> | ota check\n");
    return 1;
}

/* ==================== 初始化 ==================== */

esp_err_t ota_update_init(void)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s (OTA 不可用，其余功能不受影响)",
                 esp_err_to_name(err));
        return err;
    }

    esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "Show/set WiFi credentials for OTA",
        .hint = "[set <ssid> <pass>]",
        .func = cmd_wifi,
    };
    esp_console_cmd_t ota_cmd = {
        .command = "ota",
        .help = "OTA: set manifest url / start check",
        .hint = "url <url> | check",
        .func = cmd_ota,
    };
    esp_console_cmd_register(&wifi_cmd);
    esp_console_cmd_register(&ota_cmd);

#if CONFIG_BILIMOUSE_OTA_AUTO_CHECK
    xTaskCreate(auto_check_task, "ota_auto", 4096, NULL, 2, NULL);
#endif
    return ESP_OK;
}
