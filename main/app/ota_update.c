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

#include "hid_device.h"
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
static bool s_netif_created;   /* 默认 STA netif 是否已创建（只能创建一次，
                                  重复创建会触发 esp_netif 断言 -> 重启） */
static volatile bool s_wifi_got_ip;
static esp_event_handler_instance_t s_ev_any;
static esp_event_handler_instance_t s_ev_ip;

/* 升级状态快照与 WiFi 步骤文案（定义见「状态快照」小节） */
static void state_wifi(const char *txt);

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

/** 分步幂等的 WiFi 初始化：任何一步失败都可以安全重试。
 *  netif 只能创建一次（重复创建 -> esp_netif 断言 -> 重启），
 *  esp_wifi_init 失败后下一次调用会重试（内存可能已被别的任务释放）。 */
static esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) {
        return ESP_OK;
    }
    if (!s_netif_created) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        esp_netif_create_default_wifi_sta();
        s_netif_created = true;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
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
        /* 屏上同步显示正在试哪个热点：三个热点串行各等 9s，
           每换一个热点都有反馈，最坏 27s 内出结论。 */
        char wifi_txt[48];
        snprintf(wifi_txt, sizeof(wifi_txt), "正在连 %s", list[i].ssid);
        state_wifi(wifi_txt);
        wifi_try_cred(&list[i]);
        if (s_wifi_got_ip) {
            snprintf(wifi_txt, sizeof(wifi_txt), "已连 %s", list[i].ssid);
            state_wifi(wifi_txt);
            ESP_LOGI(TAG, "wifi connected via %s", list[i].ssid);
        }
    }
    (void)any_configured;
    return s_wifi_got_ip ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ==================== manifest ==================== */

typedef struct {
    char version[24];
    char url[384];   /* GitHub 资产的最终地址带签名参数，可能 300+ 字节 */
    char notes[96];
    int  size;       /* 升级包字节数（可选字段，旧 manifest 没有 = 0） */
} ota_manifest_t;

/** 跟随 30x 重定向解析出最终 URL。
 *  esp_http_client 的 open/read 低层 API 不自动跟随重定向，而
 *  releases/latest/download/xxx 固定链接必然 302（先跳版本化地址，
 *  再跳 objects.githubusercontent.com 签名地址），必须手动跟。 */
static esp_err_t http_resolve_url(const char *url_in, char *out, size_t outlen)
{
    char url[512];
    strlcpy(url, url_in, sizeof(url));
    for (int hop = 0; hop < 5; hop++) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 8000,
            .buffer_size = 1024,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            return ESP_FAIL;
        }
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(c);
            return err;
        }
        esp_http_client_fetch_headers(c);
        int code = esp_http_client_get_status_code(c);
        bool redirect = (code == 301 || code == 302 || code == 303 ||
                         code == 307 || code == 308);
        if (!redirect) {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            break;
        }
        /* ⚠️ 不能用 esp_http_client_get_header("Location")：
           那个 API 读的是**请求头**，响应头默认根本不保存
           （CONFIG_ESP_HTTP_CLIENT_SAVE_RESPONSE_HEADERS 默认关），
           所以 Location 永远查不到、链就断在第一跳。
           响应解析阶段 Location 已经存进 client->location，
           set_redirection 内部就是 set_url(client->location)。 */
        esp_err_t red = esp_http_client_set_redirection(c);
        if (red == ESP_OK) {
            char newurl[512] = {0};
            if (esp_http_client_get_url(c, newurl, sizeof(newurl)) == ESP_OK &&
                newurl[0]) {
                strlcpy(url, newurl, sizeof(url));
            } else {
                red = ESP_FAIL;
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (red != ESP_OK) {
            return red;
        }
        ESP_LOGI(TAG, "follow redirect %d -> %.60s%s", code, url,
                 strlen(url) > 60 ? "…" : "");
    }
    strlcpy(out, url, outlen);
    return ESP_OK;
}

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
    char url[512];
    if (!manifest_url_get(url, sizeof(url))) {
        ESP_LOGW(TAG, "manifest url empty (serial: 'ota url <http://...>')");
        return ESP_ERR_INVALID_STATE;
    }
    /* latest 固定链接是 302 链，先解析出真实的资产地址 */
    esp_err_t e = http_resolve_url(url, url, sizeof(url));
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "resolve manifest url failed: %s", esp_err_to_name(e));
        return e;
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

    if (total <= 0) {
        ESP_LOGW(TAG, "manifest http %d, %d bytes (%s)", code, total, url);
        return ESP_ERR_NOT_FOUND;
    }
    body[total] = '\0';
    if (code != 200) {
        /* 实测 GitHub release-assets（Azure CDN）对 ESP32 的请求返回过
           618 这种非标准状态码，但 body 完整可用；curl 走 PC 却是 200。
           所以这里只警告，成败交给下面的字段解析判断。 */
        ESP_LOGW(TAG, "manifest http %d (non-200), %d bytes", code, total);
        ESP_LOGI(TAG, "manifest body head: %.120s", body);
    }

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

/* ==================== 状态快照（UI 分步显示用） ==================== */

/* UI 读这份快照刷新升级页。WiFi 阶段电台等模块也会触发写，无害。 */
static ota_state_t s_state;

/* 检查成功后缓存的清单，「立即升级」直接用它，不再重复联网拉一遍 */
static ota_manifest_t s_manifest;
static bool s_manifest_valid;

/* 检查/升级任务互斥（防止连点起多个任务抢 WiFi/flash） */
static volatile bool s_task_busy;

void ota_update_get_state(ota_state_t *out)
{
    if (out) {
        *out = s_state;
    }
}

/** WiFi 步骤的实时文案（连哪个热点 / 是否连上） */
static void state_wifi(const char *txt)
{
    strlcpy(s_state.wifi_info, txt, sizeof(s_state.wifi_info));
    ui_mark_dirty();
}

/** 下载 + 写 flash + 切启动分区。成功不返回（重启）。 */
static esp_err_t ota_perform(void)
{
    /* bin 地址同样可能是个 302 链（latest/download 固定链接），
       先解析到真实的对象存储地址再交给 esp_https_ota */
    char bin_url[512];
    esp_err_t e = http_resolve_url(s_manifest.url, bin_url, sizeof(bin_url));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "resolve bin url failed: %s", esp_err_to_name(e));
        return e;
    }

    esp_http_client_config_t http_cfg = {
        .url = bin_url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* 同 manifest：支持 GitHub HTTPS */
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t h = NULL;
    s_state.phase = OTA_PHASE_DOWNLOAD;
    s_state.percent = -1;
    ui_mark_dirty();
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota begin failed: %s", esp_err_to_name(err));
        s_state.phase = OTA_PHASE_FAIL;
        return err;
    }
    /* manifest 没带 size 时，这里能拿到镜像真实大小 */
    int img_total = esp_https_ota_get_image_size(h);
    if (img_total > 0) {
        s_state.total_kb = img_total / 1024;
    }

    esp_app_desc_t desc;
    while ((err = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        if (esp_https_ota_get_image_len_read(h) >= (int)sizeof(esp_image_header_t) + (int)sizeof(esp_image_segment_header_t) &&
            esp_https_ota_get_img_desc(h, &desc) == ESP_OK) {
            ESP_LOGI(TAG, "remote image version: %s", desc.version);
        }
        int total = esp_https_ota_get_image_size(h);
        int read = esp_https_ota_get_image_len_read(h);
        s_state.total_kb = total / 1024;
        s_state.done_kb = read / 1024;
        if (total > 0) {
            s_state.percent = read * 100 / total;
        }
        power_mgr_notify_activity();   /* 别让屏幕在升级途中睡着 */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota download failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(h);
        s_state.phase = OTA_PHASE_FAIL;
        return err;
    }

    err = esp_https_ota_finish(h);   /* 内部已做镜像校验 + set_boot_partition */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota finish failed: %s", esp_err_to_name(err));
        s_state.phase = OTA_PHASE_FAIL;
        return err;
    }
    ESP_LOGI(TAG, "ota complete, boot partition switched");
    s_state.percent = 100;
    s_state.phase = OTA_PHASE_READY;
    return ESP_OK;
}

/** 升级完成/失败后的 5 秒倒计时重启（只在实际写成功后调用） */
static void ota_reboot_countdown(void)
{
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

/* ==================== 第 1 步：检查（只查不升） ==================== */

static void ota_check_run(bool auto_mode)
{
    power_mgr_notify_activity();

    memset(&s_state, 0, sizeof(s_state));
    s_state.percent = -1;
    strlcpy(s_state.cur_version, esp_app_get_description()->version,
            sizeof(s_state.cur_version));

    /* ---- 1. WiFi：失败就停在这一步，屏上明确显示 ---- */
    s_state.phase = OTA_PHASE_WIFI;
    if (!ota_update_wifi_connected()) {
        state_wifi("连接中…");
        esp_err_t e = wifi_ensure_connected();   /* 内部逐步更新 wifi_info */
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "wifi connect failed: %s", esp_err_to_name(e));
            state_wifi(e == ESP_ERR_INVALID_STATE ? "未配置（串口 wifi set）" : "连接失败");
            s_state.phase = OTA_PHASE_FAIL;
            if (auto_mode) {
                return;   /* 自动检查静默失败，不骚扰屏幕 */
            }
            ui_show_action("WiFi 未连上", "检查路由器或稍后再试");
            return;
        }
    }

    /* ---- 2. 版本清单：失败就停在这一步 ---- */
    s_state.phase = OTA_PHASE_MANIFEST;
    strlcpy(s_state.list_info, "获取中…", sizeof(s_state.list_info));
    ui_mark_dirty();
    esp_err_t e = manifest_fetch(&s_manifest);
    if (e != ESP_OK) {
        strlcpy(s_state.list_info, "获取失败", sizeof(s_state.list_info));
        s_state.phase = OTA_PHASE_FAIL;
        ESP_LOGW(TAG, "manifest fetch failed: %s", esp_err_to_name(e));
        if (auto_mode) {
            return;
        }
        ui_show_action("无法获取版本信息", "检查网络后重试");
        return;
    }
    s_manifest_valid = true;
    strlcpy(s_state.list_info, "已获取", sizeof(s_state.list_info));

    /* ---- 3. 版本比较：只有真的有新版才进入 READY ---- */
    const esp_app_desc_t *cur = esp_app_get_description();
    int cmp = semver_cmp(s_manifest.version, cur->version);
    if (cmp <= 0) {
        ESP_LOGI(TAG, "firmware up to date (%s)", cur->version);
        s_state.phase = OTA_PHASE_LATEST;
        snprintf(s_state.cmp_info, sizeof(s_state.cmp_info),
                 "已是最新(%s)", s_manifest.version);
        if (!auto_mode) {
            ui_show_action("已是最新版本", s_state.cmp_info);
        }
        return;
    }

    s_state.phase = OTA_PHASE_READY;
    strlcpy(s_state.new_version, s_manifest.version, sizeof(s_state.new_version));
    if (s_manifest.size > 0) {
        s_state.total_kb = s_manifest.size / 1024;
    }
    snprintf(s_state.cmp_info, sizeof(s_state.cmp_info), "发现新版本 %s",
             s_manifest.version);
    ESP_LOGI(TAG, "new version available: %s -> %s (%d KB)",
             cur->version, s_manifest.version, s_state.total_kb);
    if (!auto_mode) {
        ui_show_action("发现新版本", "点「立即升级」开始");
    }
    power_mgr_notify_activity();
}

/* ==================== 第 2 步：下载安装 ==================== */

static void ota_download_run(void)
{
    if (!s_manifest_valid || s_state.phase != OTA_PHASE_READY) {
        ESP_LOGW(TAG, "download without a READY check");
        s_state.phase = OTA_PHASE_FAIL;
        return;
    }

    esp_err_t err = ota_perform();
    if (err != ESP_OK) {
        ui_show_action("升级失败", "旧固件继续运行");
        return;
    }
    /* 新固件启动在 PENDING_VERIFY 状态，app_main 走完才确认；
       若新固件起不来，bootloader 自动回滚旧版本。 */
    ota_reboot_countdown();
}

/* ==================== 任务入口（防重入） ==================== */

/**
 * BLE 恢复前先把 WiFi 断开。
 *
 * 连接态的动态收发缓冲（rx/tx、TCP 窗口）会把刚还给堆的 BLE 内存
 * 重新吃掉并切碎，Bluedroid 重启时连一个小信号量都分不出来
 * （future_new unable to allocate -> assert 崩溃重启，实测踩过）。
 * 断开后回到与开机等价的内存条件（开机时 WiFi init+蓝牙 init 能共存）。
 * 恢复成功后由调用方决定是否把 WiFi 重新连上（电台要用网）。
 */
static void wifi_drop_for_ble(void)
{
    if (s_wifi_got_ip) {
        ESP_LOGI(TAG, "dropping wifi before BLE resume");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(600));   /* 等事件任务把动态缓冲放回堆 */
    }
}

static void check_task(void *arg)
{
    (void)arg;
    ota_check_run(false);
    /* 没有发现新版本（已是最新 / 失败）就把蓝牙拉回来；
       发现有新版本（READY）则保持关闭，等下载/升级完重启。 */
    if (s_state.phase != OTA_PHASE_READY) {
        wifi_drop_for_ble();
        hid_ble_resume();
        wifi_ensure_connected();   /* 电台还要用网；失败也不影响下次重试 */
    }
    s_task_busy = false;
    vTaskDelete(NULL);
}

static void download_task(void *arg)
{
    (void)arg;
    ota_download_run();
    /* 成功路径会走倒数重启，到不了这里；失败则恢复蓝牙 */
    if (s_state.phase == OTA_PHASE_FAIL) {
        wifi_drop_for_ble();
        hid_ble_resume();
        wifi_ensure_connected();
    }
    s_task_busy = false;
    vTaskDelete(NULL);
}

void ota_update_check(void)
{
    if (s_task_busy) {
        ESP_LOGW(TAG, "ota task busy, ignore");
        ui_show_hint("更新任务进行中");
        return;
    }

    /* 先把状态切到「连接 WiFi」再起任务。
       以前只有后台任务起来后才刷新屏幕，而任务一旦创建失败
       （内部 RAM 不够时 xTaskCreate 会直接返回失败）屏幕上**一点反应都没有**，
       用户看到的就是「点了开始检查更新没反应」。现在按钮一点，屏幕立刻变。 */
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = OTA_PHASE_WIFI;
    s_state.percent = -1;
    strlcpy(s_state.cur_version, esp_app_get_description()->version,
            sizeof(s_state.cur_version));
    strlcpy(s_state.wifi_info, "准备中…", sizeof(s_state.wifi_info));
    strlcpy(s_state.list_info, "未检查", sizeof(s_state.list_info));
    strlcpy(s_state.cmp_info, "未检查", sizeof(s_state.cmp_info));
    ui_mark_dirty();
    power_mgr_notify_activity();

    /* 蓝牙先整个关掉：BLE 常驻约 60~80KB 内部 RAM，OTA 的 9KB 任务栈
       在蓝牙在场时根本分不出来（check task create failed 的根因）。 */
    hid_ble_suspend();

    s_task_busy = true;
    if (xTaskCreate(check_task, "ota_chk", OTA_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_task_busy = false;
        hid_ble_resume();
        s_state.phase = OTA_PHASE_FAIL;
        strlcpy(s_state.wifi_info, "内存不足，任务起不来", sizeof(s_state.wifi_info));
        ESP_LOGE(TAG, "check task create failed, free heap=%u",
                 (unsigned)esp_get_free_heap_size());
        ui_mark_dirty();
        return;
    }
    ESP_LOGI(TAG, "check started");
}

void ota_update_download(void)
{
    if (s_task_busy) {
        ESP_LOGW(TAG, "ota task busy, ignore");
        ui_show_hint("更新任务进行中");
        return;
    }
    if (s_state.phase != OTA_PHASE_READY) {
        ui_show_hint("请先检查更新");
        return;
    }
    s_task_busy = true;
    hid_ble_suspend();   /* 与检查流程同理：下载安装前关蓝牙腾内存 */
    /* 同样先给屏幕一个即时反馈（进度条 0%），再起后台任务 */
    s_state.percent = 0;
    s_state.done_kb = 0;
    s_state.phase = OTA_PHASE_DOWNLOAD;
    ui_mark_dirty();
    if (xTaskCreate(download_task, "ota_dl", OTA_TASK_STACK, NULL, 3, NULL) != pdPASS) {
        s_task_busy = false;
        hid_ble_resume();
        s_state.phase = OTA_PHASE_FAIL;
        ESP_LOGE(TAG, "download task create failed, free heap=%u",
                 (unsigned)esp_get_free_heap_size());
        ui_mark_dirty();
    }
}

/* ==================== 自动检查 ==================== */

#if CONFIG_BILIMOUSE_OTA_AUTO_CHECK
static void auto_check_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(AUTO_CHECK_DELAY_S * 1000));
    /* 自动流程不弹错误提示（WiFi 没配好就静默跳过），只在
       确认发现新版本时才打扰用户：查到 READY 就直接继续下载。 */
    hid_ble_suspend();   /* 同样先腾内存（期间 HID 短暂不可用，几十秒内恢复） */
    ota_check_run(true);
    if (s_state.phase == OTA_PHASE_READY) {
        ota_download_run();
        if (s_state.phase == OTA_PHASE_FAIL) {
            wifi_drop_for_ble();
            hid_ble_resume();
            wifi_ensure_connected();
        }
    } else {
        wifi_drop_for_ble();
        hid_ble_resume();
        wifi_ensure_connected();
    }
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
        ota_update_check();
        printf("check started, watch the screen / serial log\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "download") == 0) {
        ota_update_download();
        printf("download started (requires a successful check first)\n");
        return 0;
    }
    printf("usage: ota url <manifest-url> | ota check | ota download\n");
    return 1;
}

/* ==================== 初始化 ==================== */

esp_err_t ota_update_init(void)
{
    esp_err_t err = wifi_init_once();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi init failed: %s (OTA 不可用，其余功能不受影响)",
                 esp_err_to_name(err));
        /* 不 return：wifi/ota 命令必须注册，否则串口连 ota check 都没有，
           而且之后 wifi_ensure_connected 里还会重试 wifi_init_once */
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
