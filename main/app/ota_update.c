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
/* 正在切换凭据（wifi_try_cred 内部）。切换期间驱动会因为 set_config
   主动断开旧 AP，若回调此时再 esp_wifi_connect() 就会和新凭据抢连接，
   导致新 SSID 一直排不上队 —— 这是备用热点连不上的直接原因之一。 */
static volatile bool s_cred_switching;
static esp_event_handler_instance_t s_ev_any;
static esp_event_handler_instance_t s_ev_ip;

/* 升级状态快照与各步骤文案（定义见「状态快照」小节） */
static void state_wifi(const char *txt);
static void state_list(const char *txt);

/** 断开原因 → 人能看懂的一句话。
 *  reason 是判断「连不上」到底卡在哪一层的唯一硬证据：
 *  201/210 = 根本没扫到这个 SSID（写错/5GHz/太远）
 *  202/15  = 扫到了但认证不过（密码错 / 需要 802.1X 或网页登录）
 *  203     = AP 拒绝关联（加密方式不匹配，如强制 WPA3/PMF） */
static const char *wifi_reason_str(int r)
{
    switch (r) {
    case WIFI_REASON_NO_AP_FOUND:
        return "扫不到该 SSID(信号弱/不在 2.4G/名字写错)";
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        return "扫到 AP 但加密方式不兼容";
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return "AP 加密低于 authmode 阈值";
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "信号低于 RSSI 阈值";
    case WIFI_REASON_AUTH_FAIL:
        return "认证被拒(密码错/需 802.1X 或网页登录)";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "四次握手超时(密码错)";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "握手超时";
    case WIFI_REASON_ASSOC_FAIL:
        return "关联被拒(AP 不接受该加密/需 PMF)";
    case WIFI_REASON_CONNECTION_FAIL:
        return "连接失败";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "信标丢失(信号中断)";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC 校验失败(密码错)";
    case WIFI_REASON_AUTH_EXPIRE:
        return "认证超时";
    default:
        return "其他原因";
    }
}

static void wifi_event_cb(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_wifi_got_ip = false;
        if (d) {
            /* v6 的 disconnected 事件没有 channel 字段，ssid 也不保证
               以 '\0' 结尾，必须按 ssid_len 打印。 */
            ESP_LOGW(TAG, "wifi disconnected: reason=%u (%s) ssid='%.*s' rssi=%d",
                     (unsigned)d->reason, wifi_reason_str(d->reason),
                     (int)d->ssid_len, (const char *)d->ssid, d->rssi);
        }
        /* 只在「当前凭据的自动重试」场景里重连；切凭据期间由
           wifi_try_cred 自己控制 connect，避免两路抢同一个 STA。 */
        if (!s_cred_switching) {
            esp_wifi_connect();
        }
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

/** 打印候选凭据的真实字节，用来抓「看不见」的问题：
 *  中文 SSID 被 Kconfig 存成 GBK/乱码、末尾多一个空格、全角字符等。
 *  这类问题密码再对也连不上，而且日志里肉眼看不出差别。 */
static void creds_dump(const wifi_cred_t *list, int n)
{
    for (int i = 0; i < n; i++) {
        char hex[3 * 34 + 1];
        int k = 0;
        for (const unsigned char *p = (const unsigned char *)list[i].ssid;
             *p && k < (int)sizeof(hex) - 4; p++) {
            k += snprintf(hex + k, sizeof(hex) - (size_t)k, "%02X ", *p);
        }
        hex[k] = '\0';
        ESP_LOGI(TAG, "cred[%d] ssid='%s' (%uB) hex=%s pass_len=%u",
                 i, list[i].ssid, (unsigned)strlen(list[i].ssid), hex,
                 (unsigned)strlen(list[i].pass));
    }
}

/** 对单个凭据尝试连接。成功返回 true 并保持连接。 */
static bool wifi_try_cred(const wifi_cred_t *c)
{
    /* 1) 先把上一轮的状态彻底清掉。
          不 disconnect 直接 set_config，驱动可能还挂在旧 AP 的
          connect 流程里，新的 SSID 永远发不出 probe。 */
    s_cred_switching = true;
    esp_wifi_disconnect();          /* 未连接时返回 ERR_WIFI_NOT_CONNECT，忽略 */
    vTaskDelay(pdMS_TO_TICKS(150));
    s_wifi_got_ip = false;

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;   /* 不设最低加密要求 */
    /* 2) 现代 AP（含手机热点）几乎都开 PMF / WPA3。
          默认 pmf_cfg.capable=false，AP 会直接拒关联（reason 203/30），
          表现就是「密码明明对，就是连不上」。 */
    wc.sta.pmf_cfg.capable  = true;
    wc.sta.pmf_cfg.required = false;   /* 混合模式仍允许走纯 WPA2 */
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_config(%s) failed: %s", c->ssid, esp_err_to_name(err));
        s_cred_switching = false;
        return false;
    }

    /* 3) 单凭据内重试 2 轮（每轮 5s）。ESP-IDF 不会自己重连，
          只打一次 connect 的话一次失败就永远错过这个热点。 */
    for (int attempt = 0; attempt < 2 && !s_wifi_got_ip; attempt++) {
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGW(TAG, "connect(%s) attempt %d: %s", c->ssid, attempt + 1,
                     esp_err_to_name(err));
        }
        for (int waited = 0; waited < (WIFI_TIMEOUT_MS / 2) && !s_wifi_got_ip;
             waited += 200) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (!s_wifi_got_ip) {
        esp_wifi_disconnect();
    }
    s_cred_switching = false;
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

    wifi_cred_t list[4];
    bool nvs_used = false;
    int n = creds_collect(list, 4, &nvs_used);
    if (n == 0) {
        ESP_LOGW(TAG, "no wifi credentials (serial: 'wifi set <ssid> <pass>')");
        return ESP_ERR_INVALID_STATE;
    }
    /* 槽位 = 1 个 NVS + 3 个编译期默认。之前只有 3 个槽，一旦用过
       `wifi set`，第三个默认热点会被挤出去，永远没机会尝试。 */
    creds_dump(list, n);

    for (int i = 0; i < n && !s_wifi_got_ip; i++) {
        ESP_LOGI(TAG, "wifi connecting [%d/%d]: '%s'%s", i + 1, n, list[i].ssid,
                 (i == 0 && nvs_used) ? " (NVS)" : "");
        /* 屏上同步显示正在试哪个热点：逐个串行尝试，每次都有反馈，
           最坏 3 个 × 2 轮 × 5s = 30s 内出结论。 */
        char wifi_txt[48];
        snprintf(wifi_txt, sizeof(wifi_txt), "正在连 %s", list[i].ssid);
        state_wifi(wifi_txt);
        wifi_try_cred(&list[i]);
        if (s_wifi_got_ip) {
            snprintf(wifi_txt, sizeof(wifi_txt), "已连 %s", list[i].ssid);
            state_wifi(wifi_txt);
            ESP_LOGI(TAG, "wifi connected via '%s'", list[i].ssid);
        }
    }
    if (!s_wifi_got_ip) {
        ESP_LOGW(TAG, "all %d credentials failed (看上面 reason= 行定位)", n);
        state_wifi("WiFi 连接失败");
    }
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
            /* 5s：单跳重定向只需要读到响应头，5s 已经足够。
               原来 8s，多源串行时会把"卡着"的观感放大。 */
            .timeout_ms = 5000,
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

/* 单次抓取 manifest 并解析 JSON（不含 URL 解析与重试）。 */
static esp_err_t manifest_fetch_once(ota_manifest_t *m, const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        /* 7s：TLS 握手 + 读一个 1KB 以内的 JSON。带证书包时握手本身
           在纯软件大数运算下要 2~4s，7s 留足余量又不会让屏幕长时间不动。 */
        .timeout_ms = 7000,
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

/** 内置 manifest 源，按「国内可达性」排序。
 *
 *  为什么必须多源：releases/latest/download 会 302 到
 *  objects.githubusercontent.com，raw.githubusercontent.com 也一样，
 *  这两个域名在国内（尤其手机热点/公司访客网）经常被 DNS 污染或直接
 *  超时。以前只有单源 + 3 次重试 × 10s，最坏 60 秒屏幕一动不动 ——
 *  用户看到的就是「检查更新一直卡着」。
 *
 *  现在改成多源串行、每个源 5~7s 超时，屏上实时显示「第几个源」，
 *  某个源不通立刻换下一个（jsDelivr 的国内可达性通常明显好于 raw）。 */
static const char *const k_manifest_srcs[] = {
    /* 1. 直连 raw：最新、最快（能通就 1s 内出结果） */
    "https://raw.githubusercontent.com/ChenadSH/bilimouse-esp32s3/master/ota/manifest.json",
    /* 2. gh-proxy 反代 raw：国内/公司网直连 GitHub 被墙时通常可用，
       而且和直连一样是「实时内容」，不会读到旧版本。 */
    "https://gh-proxy.com/https://raw.githubusercontent.com/ChenadSH/bilimouse-esp32s3/master/ota/manifest.json",
    /* 3. jsDelivr：可用性最好，但分支引用有缓存（可能滞后几小时），放第 3 */
    "https://cdn.jsdelivr.net/gh/ChenadSH/bilimouse-esp32s3@master/ota/manifest.json",
    /* 4. Releases 固定链接：会 302 到 objects.githubusercontent.com */
    "https://github.com/ChenadSH/bilimouse-esp32s3/releases/latest/download/manifest.json",
};

/**
 * manifest 获取（多源串行 + 每个源失败后换源）。
 *
 * GitHub 资产走 Azure Front Door CDN，它对 ESP-IDF 的 TLS 指纹有 bot
 * 防护，会**间歇性**返回 618 + XHTML 拒绝页；国内网络还会直接超时。
 * 所以策略是「换域名」而不是「同一个域名死磕三次」。
 */
static esp_err_t manifest_fetch(ota_manifest_t *m)
{
    const char *srcs[5];
    int n = 0;

    /* NVS 里手动配的 URL（ota url ...）优先，便于自建镜像。
       manifest_url_get() 在没配时返回编译期默认值，可能与下面的候选重复，
       所以统一做一次去重。 */
    char configured[512];
    if (manifest_url_get(configured, sizeof(configured))) {
        srcs[n++] = configured;
    }
    for (int i = 0; i < (int)(sizeof(k_manifest_srcs) / sizeof(k_manifest_srcs[0])) &&
                    n < (int)(sizeof(srcs) / sizeof(srcs[0])); i++) {
        bool dup = false;
        for (int k = 0; k < n; k++) {
            if (strcmp(srcs[k], k_manifest_srcs[i]) == 0) {
                dup = true;
            }
        }
        if (!dup) {
            srcs[n++] = k_manifest_srcs[i];
        }
    }

    esp_err_t last = ESP_FAIL;
    for (int i = 0; i < n; i++) {
        char txt[40];
        /* 屏幕上必须能看出"在动"：卡在「获取中…」和卡在「获取中…(2/3)」
           对用户是完全不同的体验，后者能证明固件还活着。 */
        snprintf(txt, sizeof(txt), "获取中…(%d/%d)", i + 1, n);
        state_list(txt);

        char resolved[512];
        strlcpy(resolved, srcs[i], sizeof(resolved));
        last = http_resolve_url(resolved, resolved, sizeof(resolved));
        if (last == ESP_OK) {
            last = manifest_fetch_once(m, resolved);
        }
        if (last == ESP_OK) {
            ESP_LOGI(TAG, "manifest ok from source %d/%d", i + 1, n);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "manifest source %d/%d failed: %s (%.60s)", i + 1, n,
                 esp_err_to_name(last), srcs[i]);
        snprintf(txt, sizeof(txt), "源%d失败", i + 1);
        state_list(txt);
    }
    return last;
}

/* ==================== 状态快照（UI 分步显示用） ==================== */

/* UI 读这份快照刷新升级页。WiFi 阶段电台等模块也会触发写，无害。 */
static ota_state_t s_state;

/* 检查成功后缓存的清单，「立即升级」直接用它，不再重复联网拉一遍 */
static ota_manifest_t s_manifest;
static bool s_manifest_valid;

/* 检查/升级任务互斥（防止连点起多个任务抢 WiFi/flash） */
static volatile bool s_task_busy;

/* 收尾阶段的 WiFi 重连不更新屏上文案（否则会把检查结论刷掉） */
static volatile bool s_wifi_quiet;

void ota_update_get_state(ota_state_t *out)
{
    if (out) {
        *out = s_state;
    }
}

/** WiFi 步骤的实时文案（连哪个热点 / 是否连上） */
static void state_wifi(const char *txt)
{
    /* 后台重连时不要覆盖屏幕上的结论（「已是最新」→「正在连 xxx」）。
       检查结束后要把 WiFi 拉回来，那是收尾动作，用户不需要看。 */
    if (s_wifi_quiet) {
        return;
    }
    strlcpy(s_state.wifi_info, txt, sizeof(s_state.wifi_info));
    ui_mark_dirty();
}

/** 清单步骤的实时文案（正在试第几个源） */
static void state_list(const char *txt)
{
    strlcpy(s_state.list_info, txt, sizeof(s_state.list_info));
    ui_mark_dirty();
}

/** 用已经解析好的最终地址下载 + 写 flash + 切启动分区。 */
static esp_err_t ota_perform_url(const char *bin_url)
{
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

    /* bin 下载同样可能被 CDN 的 bot 防护间歇性拒绝（618），失败重试。
       原来重试 3 次，现在外层还会换源（jsDelivr / raw），
       单源重试降到 2 次，避免「坏源上死磕 3 次 × 40s」把整体拖成几分钟。 */
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= 2; attempt++) {
        h = NULL;
        err = esp_https_ota_begin(&ota_cfg, &h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ota begin failed (attempt %d/2): %s", attempt,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
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
            ESP_LOGW(TAG, "ota download failed (attempt %d/2): %s", attempt,
                     esp_err_to_name(err));
            esp_https_ota_abort(h);
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }

        err = esp_https_ota_finish(h);   /* 内部已做镜像校验 + set_boot_partition */
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ota finish failed (attempt %d/2): %s", attempt,
                     esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1500));
            continue;
        }
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota failed: %s", esp_err_to_name(err));
        s_state.phase = OTA_PHASE_FAIL;
        return err;
    }
    ESP_LOGI(TAG, "ota complete, boot partition switched");
    s_state.percent = 100;
    s_state.phase = OTA_PHASE_READY;
    return ESP_OK;
}

/**
 * 生成第 idx 个 bin 下载候选地址（写入 out）。
 * 返回 false 表示没有这个候选（例如 manifest 没带 version）。
 *
 * 为什么要多个：manifest 能取到、bin 下不下来，是这套 OTA 最容易卡死的一步。
 * 候选里既要有「原地址」，也要有反代/镜像，否则一旦 raw 被墙，升级永远走不通。
 */
static bool bin_candidate(int idx, char *out, size_t len)
{
    switch (idx) {
    case 0:   /* manifest 里给的原地址（raw 的 tag 路径，最权威） */
        if (s_manifest.url[0] == '\0') {
            return false;
        }
        strlcpy(out, s_manifest.url, len);
        return true;
    case 1:   /* gh-proxy 反代同一条地址：内容实时，国内可达性最好 */
        if (s_manifest.url[0] == '\0') {
            return false;
        }
        snprintf(out, len, "https://gh-proxy.com/%s", s_manifest.url);
        return true;
    case 2:   /* jsDelivr 按版本号：发布后要等 tag 缓存刷新，放最后 */
        if (s_manifest.version[0] == '\0') {
            return false;
        }
        snprintf(out, len,
                 "https://cdn.jsdelivr.net/gh/ChenadSH/bilimouse-esp32s3@v%s/ota/bilimouse.bin",
                 s_manifest.version);
        return true;
    default:
        return false;
    }
}

/**
 * 下载升级包：按候选列表逐个尝试，全部失败才报错。
 */
static esp_err_t ota_perform(void)
{
    esp_err_t err = ESP_FAIL;
    bool tried = false;

    for (int i = 0; i < 3; i++) {
        char cand[512];
        if (!bin_candidate(i, cand, sizeof(cand))) {
            continue;
        }
        tried = true;

        char real[512];
        err = http_resolve_url(cand, real, sizeof(real));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "resolve bin url %d/3 failed: %s", i + 1,
                     esp_err_to_name(err));
            continue;
        }
        err = ota_perform_url(real);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "bin source %d/3 failed: %s", i + 1, esp_err_to_name(err));
    }

    if (!tried) {
        return ESP_ERR_INVALID_STATE;
    }
    s_state.phase = OTA_PHASE_FAIL;
    return err;
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
    /* 注意：不能校验 phase == READY —— ota_update_download() 起任务前
       已经把 phase 置成 OTA_PHASE_DOWNLOAD（为了让屏幕立刻显示进度条），
       这里再要求 READY 会永远不成立（表现为「download without a READY
       check」，升级按钮点了没反应）。真正的前提只有「manifest 有效」。 */
    if (!s_manifest_valid) {
        ESP_LOGW(TAG, "download without a valid manifest");
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

    /* 检查结论已经出来了，先把任务闸门放开：下面的蓝牙恢复 + WiFi 重连
       可能要十几秒，这期间用户再点「开始检查更新」不该被「任务进行中」
       挡回去（以前 s_task_busy 一路占到最后，看起来就像整个流程卡死）。 */
    s_task_busy = false;

    /* 没有发现新版本（已是最新 / 失败）就把蓝牙拉回来；
       发现有新版本（READY）则保持关闭，等下载/升级完重启。 */
    if (s_state.phase != OTA_PHASE_READY) {
        s_wifi_quiet = true;
        wifi_drop_for_ble();
        hid_ble_resume();
        wifi_ensure_connected();   /* 电台还要用网；失败也不影响下次重试 */
        s_wifi_quiet = false;
    }
    vTaskDelete(NULL);
}

static void download_task(void *arg)
{
    (void)arg;
    ota_download_run();
    /* 成功路径会走倒数重启，到不了这里；失败则恢复蓝牙 */
    if (s_state.phase == OTA_PHASE_FAIL) {
        s_task_busy = false;
        s_wifi_quiet = true;
        wifi_drop_for_ble();
        hid_ble_resume();
        wifi_ensure_connected();
        s_wifi_quiet = false;
    } else {
        s_task_busy = false;
    }
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

static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN(无密码)";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK(老)";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2-PSK";
    case WIFI_AUTH_ENTERPRISE:      return "WPA2-ENTERPRISE(要域账号,固件不支持)";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3-PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI-PSK";
    case WIFI_AUTH_OWE:             return "OWE";
    case WIFI_AUTH_WPA3_ENT_192:    return "WPA3-ENT-192(不支持)";
    case WIFI_AUTH_WPA3_ENTERPRISE: return "WPA3-ENTERPRISE(不支持)";
    case WIFI_AUTH_WPA_ENTERPRISE:  return "WPA-ENTERPRISE(不支持)";
    default:                        return "UNKNOWN";
    }
}

/** 扫一下周围有哪些热点、用什么加密。
 *  这是回答「为什么某个 SSID 连不上」的唯一硬证据：
 *  - 列表里根本没有它   → SSID 写错 / 是 5GHz / 太远
 *  - 有但显示 ENTERPRISE → 需要 802.1X 域账号，本固件不支持
 *  - 有且 WPA3           → 需要 PMF（已在 try_cred 里开）
 *  - RSSI < -85          → 信号太弱 */
static int cmd_wifi_scan(void)
{
    if (wifi_init_once() != ESP_OK) {
        printf("wifi init failed\n");
        return 1;
    }
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        printf("scan failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    uint16_t total = 0;
    esp_wifi_scan_get_ap_num(&total);
    uint16_t want = total > 15 ? 15 : total;
    wifi_ap_record_t *recs = calloc(want ? want : 1, sizeof(wifi_ap_record_t));
    if (!recs) {
        printf("oom\n");
        esp_wifi_clear_ap_list();
        return 1;
    }
    if (want) {
        esp_wifi_scan_get_ap_records(&want, recs);
    }
    printf("found %u AP(s), showing %u (RSSI 越接近 0 越强)\n", total, want);
    printf("%-28s %3s %5s  %s\n", "SSID", "CH", "RSSI", "AUTH");
    for (int i = 0; i < want; i++) {
        printf("%-28s %3d %5d  %s\n", (const char *)recs[i].ssid,
               recs[i].primary, recs[i].rssi, authmode_str(recs[i].authmode));
    }
    free(recs);
    printf("--- 对照一下配置里的 SSID ---\n");
    wifi_cred_t list[4];
    bool nvs_used = false;
    int n = creds_collect(list, 4, &nvs_used);
    for (int i = 0; i < n; i++) {
        printf("  #%d '%s'%s\n", i + 1, list[i].ssid, (i == 0 && nvs_used) ? " (NVS)" : "");
    }
    wifi_ensure_connected();   /* 扫完恢复连接 */
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        wifi_cred_t list[4];
        bool nvs_used = false;
        int n = creds_collect(list, 4, &nvs_used);
        printf("wifi %s\n", s_wifi_got_ip ? "connected" : "not connected");
        for (int i = 0; i < n; i++) {
            printf("  #%d '%s' pass_len=%u%s\n", i + 1, list[i].ssid,
                   (unsigned)strlen(list[i].pass),
                   (i == 0 && nvs_used) ? " (NVS, 优先)" : "");
        }
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "scan") == 0) {
        return cmd_wifi_scan();
    }
    if (argc == 3 && strcmp(argv[1], "clear") == 0) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            printf("error: nvs not ready\n");
            return 1;
        }
        nvs_erase_key(h, NVS_KEY_SSID);
        nvs_erase_key(h, NVS_KEY_PASS);
        nvs_commit(h);
        nvs_close(h);
        printf("ok: NVS credentials cleared, 回落到编译期默认\n");
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
        printf("ok: credentials saved (ssid %uB, pass %uB), connecting...\n",
               (unsigned)strlen(argv[2]), (unsigned)strlen(argv[3]));
        s_wifi_got_ip = false;
        wifi_ensure_connected();
        printf("wifi %s\n", s_wifi_got_ip ? "connected" : "FAILED (看 reason= 日志)");
        return 0;
    }
    printf("usage: wifi | wifi scan | wifi set <ssid> <pass> | wifi clear\n");
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
        .help = "Show/scan/set WiFi credentials for OTA",
        .hint = "[scan | set <ssid> <pass> | clear]",
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
