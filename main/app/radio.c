/**
 * @file radio.c
 * @brief WiFi 网络电台：GitHub 台列表 + MP3/HLS 直播流播放
 *
 * 数据流：
 *   raw.githubusercontent 台列表(每次实时拉) -> 选台 -> esp_http_client 拉流
 *   -> 32KB 线性缓冲 -> libhelix MP3 帧解码 -> I2S(ES8311) -> 喇叭
 *
 * 两种流：
 *   1. MP3 直播流（蜻蜓等）：直接把字节流喂给解码器；
 *   2. HLS (m3u8)：master playlist -> media playlist -> 循环取 TS 分段，
 *      字节同样喂 MP3 解码器（Helix 逐字节同步扫描能跳过 TS 封装头）。
 *      一个 playlist 播完就重新拉最新的（按指纹跳过已播分段）。
 *
 * 并发模型（刻意保持最简）：
 *   只有一个 radio 任务。空闲时轮询请求字；播放时在帧边界（约 26ms）
 *   检查请求字，所以「停止/换台」最迟 26ms 响应，且全程无锁。
 *   UI/串口只写 s_req（volatile），绝不直接碰播放内部状态。
 */
#include "radio.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "bsp_audio.h"
#include "ota_update.h"
#include "ui.h"
#include "mp3dec.h"

#include "esp_console.h"

static const char *TAG = "radio";

/* 台列表：仓库 master 分支，永远最新（想加台改文件提交即可，不用烧固件）。
   多源容灾：raw.githubusercontent.com 在国内时常连不上，
   jsdelivr 是 GitHub 内容的国内可达 CDN，逐个试直到成功。 */
static const char *s_radio_list_urls[] = {
    "https://raw.githubusercontent.com/ChenadSH/bilimouse-esp32s3/master/radio/radio_stations.json",
    "https://cdn.jsdelivr.net/gh/ChenadSH/bilimouse-esp32s3@master/radio/radio_stations.json",
    "https://fastly.jsdelivr.net/gh/ChenadSH/bilimouse-esp32s3@master/radio/radio_stations.json",
};
#define RADIO_LIST_URL_CNT  (sizeof(s_radio_list_urls) / sizeof(s_radio_list_urls[0]))

/* 内置默认台（写死在固件里）。
 *
 * 为什么必须有：在线列表要「WiFi 连上 + GitHub 可达 + JSON 解析成功」三件事
 * 全对才有台，任何一环出问题屏幕上就只剩「台列表未就绪」——用户按播放毫无
 * 反应，看起来就是坏了。内置台让电台永远有东西可播：连不上网时它会明确
 * 报「WiFi 没连上」，而不是装作在拉列表。
 *
 * ⚠️ 必须是 MP3 直播流：CNR 的 m3u8 是 AAC 编码，Helix 只能解 MP3，
 * 放 HLS 进来只会弹「可能不是 MP3 编码」。以下三个源都实测 audio/mpeg。 */
static const radio_station_t s_default_stations[] = {
    { "中国之声",   "https://lhttp.qtfm.cn/live/15318317/64k.mp3" },
    { "上海动感101", "http://lhttp.qingting.fm/live/274/64k.mp3" },
    { "广东音乐之声", "https://lhttp.qtfm.cn/live/1260/64k.mp3" },
};
#define DEFAULT_ST_CNT  (sizeof(s_default_stations) / sizeof(s_default_stations[0]))

#define BUF_CAP      (32 * 1024)
#define HIGH_WATER   (BUF_CAP - 8 * 1024)   /* 高于此值暂停网络读 */
#define DECODE_MIN   2048                    /* 至少攒这么多再解码 */
#define READ_CHUNK   4096
/* 拉列表任务堆栈：任务是 HTTPS（mbedtls 握手要 6~8KB），5KB 会栈溢出。
   之前「台列表读不到/进电台就崩」的帮凶之一。 */
#define RADIO_LIST_STACK  (10 * 1024)

/* ==================== 状态 ==================== */

/* 台结构与拉列表的文本缓冲都放 PSRAM：内部 RAM 要留给 WiFi/BLE/LCD DMA，
   v2.4 里这三个静态数组合计近 20KB，曾直接把 esp_wifi_init 挤到 NO_MEM */
static radio_station_t s_st[RADIO_MAX_STATIONS] EXT_RAM_BSS_ATTR;
/* 解析暂存区：在线列表先解析到这里，整体成功才拷进 s_st。
   直接往 s_st 里解析会在中途把 s_cnt 清 0，此时若正在播放，
   换台算 s_cur % s_cnt 就是除零 —— 直接 panic。 */
static radio_station_t s_stage[RADIO_MAX_STATIONS] EXT_RAM_BSS_ATTR;
static int s_cnt;
static int s_cur;
static char s_status[48];

/* 拉取 HTTP 文本的复用缓冲（台列表 ~2KB、playlist ~4KB） */
static char s_http_body[8192] EXT_RAM_BSS_ATTR;
/* playlist 单独一份：台列表刷新和播放可能并发，共用一块缓冲会互相踩 */
static char s_pl_body[4096] EXT_RAM_BSS_ATTR;

/* 请求字：UI/串口写，radio 任务消费 */
typedef enum {
    REQ_NONE = 0,
    REQ_TOGGLE,   /* 空闲=播放当前台，播放中=停止 */
    REQ_NEXT,
    REQ_PREV,
    REQ_SELECT,   /* 播放 s_cur 指向的台（台列表点选） */
} radio_req_t;
static volatile radio_req_t s_req;

static TaskHandle_t s_task;
static volatile bool s_list_loading;

/* 线性缓冲 + 解码输出 */
static uint8_t *s_buf;            /* PSRAM */
static int s_len;
static int16_t *s_pcm;            /* 1152*2 帧输出 */
static HMP3Decoder s_dec;

/* 已播分段指纹（HLS 直播去重）：FNV-1a 环形表 */
#define PLAYED_N 16
static uint32_t s_played[PLAYED_N];
static int s_played_pos;

/* ==================== 小工具 ==================== */

static void set_status(const char *txt)
{
    strlcpy(s_status, txt, sizeof(s_status));
    ui_mark_dirty();
}

/** 写入内置默认台。只在台列表为空时生效 —— 绝不覆盖已拉到的在线列表。 */
static void seed_defaults(void)
{
    if (s_cnt > 0) {
        return;
    }
    int n = (int)DEFAULT_ST_CNT;
    if (n > RADIO_MAX_STATIONS) {
        n = RADIO_MAX_STATIONS;
    }
    memcpy(s_st, s_default_stations, (size_t)n * sizeof(s_st[0]));
    s_cnt = n;
    s_cur = 0;
    ESP_LOGI(TAG, "seeded %d built-in station(s)", n);
}

void radio_status_text(char *buf, int len)
{
    strlcpy(buf, s_status, len);
}

int radio_station_count(void) { return s_cnt; }
const radio_station_t *radio_station(int idx) { return (idx >= 0 && idx < s_cnt) ? &s_st[idx] : NULL; }
int radio_current_index(void) { return s_cnt ? s_cur : -1; }
bool radio_is_playing(void) { return strstr(s_status, "播放中") != NULL; }

static void show_station(const char *status)
{
    char title[40];
    if (s_cnt) {
        strlcpy(title, s_st[s_cur].name, sizeof(title));
    } else {
        strlcpy(title, "电台", sizeof(title));
    }
    ui_show_action(title, status);
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static void mark_played(const char *url)
{
    s_played[s_played_pos] = fnv1a(url);
    s_played_pos = (s_played_pos + 1) % PLAYED_N;
}

static bool was_played(const char *url)
{
    uint32_t h = fnv1a(url);
    for (int i = 0; i < PLAYED_N; i++) {
        if (h && s_played[i] == h) {
            return true;
        }
    }
    return false;
}

/* ==================== 台列表拉取 ==================== */

/** 把 JSON 字符串里的转义还原。
 *
 *  以前只处理 \" 和 \/：带鉴权参数的流地址里常见 \u0026（&），
 *  不还原就会拿一个带字面量 "\u0026" 的 URL 去请求，服务器回 400/403。 */
static void json_unescape(char *s)
{
    char *w = s;
    while (*s) {
        if (*s != '\\' || s[1] == '\0') {
            *w++ = *s++;
            continue;
        }
        switch (s[1]) {
        case '"':  *w++ = '"';  s += 2; break;
        case '\\': *w++ = '\\'; s += 2; break;
        case '/':  *w++ = '/';  s += 2; break;
        case 'n':  *w++ = '\n'; s += 2; break;
        case 'r':  *w++ = '\r'; s += 2; break;
        case 't':  *w++ = '\t'; s += 2; break;
        case 'b':  *w++ = '\b'; s += 2; break;
        case 'f':  *w++ = '\f'; s += 2; break;
        case 'u': {
            unsigned cp = 0;
            bool ok = true;
            for (int i = 0; i < 4; i++) {
                char h = s[2 + i];
                unsigned v;
                if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
                else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
                else { ok = false; break; }
                cp = cp * 16 + v;
            }
            if (!ok) {                 /* 假转义：原样保留反斜杠 */
                *w++ = *s++;
                break;
            }
            s += 6;
            /* UTF-16 代理对（\uD83D\uDE00 这类）合成一个码点 */
            if (cp >= 0xD800 && cp <= 0xDBFF && s[0] == '\\' && s[1] == 'u') {
                unsigned lo = 0;
                bool ok2 = true;
                for (int i = 0; i < 4; i++) {
                    char h = s[2 + i];
                    unsigned v;
                    if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
                    else { ok2 = false; break; }
                    lo = lo * 16 + v;
                }
                if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    s += 6;
                }
            }
            if (cp < 0x80) {
                *w++ = (char)cp;
            } else if (cp < 0x800) {
                *w++ = (char)(0xC0 | (cp >> 6));
                *w++ = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                *w++ = (char)(0xE0 | (cp >> 12));
                *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *w++ = (char)(0x80 | (cp & 0x3F));
            } else {
                *w++ = (char)(0xF0 | (cp >> 18));
                *w++ = (char)(0x80 | ((cp >> 12) & 0x3F));
                *w++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                *w++ = (char)(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            *w++ = *s++;               /* 未知转义：保留原字符 */
            break;
        }
    }
    *w = '\0';
}

static const char *json_skip_ws(const char *p)
{
    while (*p && (unsigned char)*p <= ' ') {
        p++;
    }
    return p;
}

/** p 指向开引号；start/len 返回内容（不含引号），返回闭引号之后的位置 */
static const char *json_scan_string(const char *p, const char **start, int *len)
{
    if (*p != '"') {
        return NULL;
    }
    const char *s = p + 1;
    const char *q = s;
    while (*q) {
        if (*q == '\\' && q[1]) {
            q += 2;
            continue;
        }
        if (*q == '"') {
            *start = s;
            *len = (int)(q - s);
            return q + 1;
        }
        q++;
    }
    return NULL;
}

/** 跳过一个 JSON 值（字符串/对象/数组/数字/字面量），返回其后位置 */
static const char *json_skip_value(const char *p)
{
    p = json_skip_ws(p);
    if (*p == '"') {
        const char *s;
        int l;
        return json_scan_string(p, &s, &l);
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        bool in_str = false;
        while (*p) {
            if (in_str) {
                if (*p == '\\' && p[1]) {
                    p += 2;
                    continue;
                }
                if (*p == '"') {
                    in_str = false;
                }
            } else if (*p == '"') {
                in_str = true;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if (*p == '}' || *p == ']') {
                if (--depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') {
        p++;
    }
    return p;
}

/**
 * 解析单个电台对象。p 指向 '{'，返回对象结束（'}' 之后）的位置。
 *
 * 逐键取值、按对象边界收尾 —— 老实现是「从 name 的结束引号往后
 * strstr("stream_url")」，一旦某个对象缺 stream_url（或键序反了），
 * 就会把**下一个台**的地址张冠李戴，整张列表全部错位。
 */
static const char *parse_station_obj(const char *p, char *name, size_t name_cap,
                                     char *url, size_t url_cap)
{
    const char *q = p + 1;
    for (;;) {
        q = json_skip_ws(q);
        if (*q == ',') {
            q++;
            continue;
        }
        if (*q == '}' || *q == '\0') {
            q += (*q == '}');
            break;
        }
        if (*q != '"') {          /* 结构异常：放弃这个对象 */
            break;
        }

        const char *ks;
        int kl;
        const char *next = json_scan_string(q, &ks, &kl);
        if (!next) {
            break;
        }
        q = json_skip_ws(next);
        if (*q != ':') {
            break;
        }
        q = json_skip_ws(q + 1);

        bool is_name = (kl == 4 && strncmp(ks, "name", 4) == 0);
        bool is_url = (kl == 10 && strncmp(ks, "stream_url", 10) == 0) ||
                      (kl == 3 && strncmp(ks, "url", 3) == 0);

        if (is_name || is_url) {
            const char *vs;
            int vl;
            const char *ve = (*q == '"') ? json_scan_string(q, &vs, &vl) : NULL;
            if (ve && vl > 0) {
                char *out = is_name ? name : url;
                size_t cap = is_name ? name_cap : url_cap;
                size_t n = (size_t)vl < cap - 1 ? (size_t)vl : cap - 1;
                memcpy(out, vs, n);
                out[n] = '\0';
                json_unescape(out);
            }
            q = ve ? ve : json_skip_value(q);
        } else {
            q = json_skip_value(q);
        }
        if (!q) {
            break;
        }
    }
    return q;
}

/**
 * 解析台列表。支持 `{"radio_stations":[...]}` 和裸数组两种形态。
 *
 * 只接受 MP3 直连台：Helix 只能解 MP3，国内 m3u8 直播基本都是
 * AAC/TS，收进来只会播两秒就报「不是 MP3 编码」——那正是「拉完在线
 * 列表反而全部不能播」的原因。全部条目都被滤掉时返回失败，调用方
 * 保留内置可用台，绝不用一份播不了的列表覆盖掉能播的。
 */
static esp_err_t parse_stations(char *body, radio_station_t *dst, int *out_cnt)
{
    int cnt = 0;
    int skipped_hls = 0;
    const char *p = strchr(body, '[');

    if (!p) {                      /* 没有数组：当成单对象列表 */
        p = body;
    } else {
        p++;
    }

    while (*p) {
        p = json_skip_ws(p);
        if (*p == ']') {
            break;
        }
        if (*p == ',') {
            p++;
            continue;
        }
        if (*p != '{') {           /* 数组里的非对象元素：整段跳过 */
            const char *n = json_skip_value(p);
            if (!n || n <= p) {
                break;
            }
            p = n;
            continue;
        }

        char name[sizeof(dst[0].name)] = {0};
        char url[sizeof(dst[0].url)] = {0};
        const char *n = parse_station_obj(p, name, sizeof(name), url, sizeof(url));
        if (!n || n <= p) {
            break;
        }
        p = n;

        if (!name[0] || !url[0]) {
            continue;
        }
        if (strstr(url, ".m3u8")) {
            skipped_hls++;
            ESP_LOGW(TAG, "skip hls station '%s' (helix decodes mp3 only)", name);
            continue;
        }
        if (cnt >= RADIO_MAX_STATIONS) {
            break;
        }
        strlcpy(dst[cnt].name, name, sizeof(dst[cnt].name));
        strlcpy(dst[cnt].url, url, sizeof(dst[cnt].url));
        ESP_LOGI(TAG, "station[%d] %s | %s", cnt, dst[cnt].name, dst[cnt].url);
        cnt++;
    }

    *out_cnt = cnt;
    if (cnt == 0) {
        ESP_LOGW(TAG, "no playable mp3 station (hls skipped: %d)", skipped_hls);
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static esp_err_t radio_fetch_list_one(const char *url_in)
{
    char url[512];
    strlcpy(url, url_in, sizeof(url));

    int code = 0;
    int n = 0;
    char *body = s_http_body;
    const size_t cap = sizeof(s_http_body);
    body[0] = '\0';

    /* 跟随 301/302（jsDelivr / releases 会跳）；手动 open 模式下
       esp_http_client 不会自己跟，必须自己解析 Location 重开。 */
    for (int hop = 0; hop < 3; hop++) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 8000,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t cli = esp_http_client_init(&cfg);
        if (!cli) {
            return ESP_FAIL;
        }
        esp_err_t err = esp_http_client_open(cli, 0);
        if (err != ESP_OK) {
            esp_http_client_cleanup(cli);
            return err;
        }
        esp_http_client_fetch_headers(cli);
        code = esp_http_client_get_status_code(cli);

        if (code == 301 || code == 302 || code == 303 ||
            code == 307 || code == 308) {
            char next[512] = {0};
            esp_err_t red = esp_http_client_set_redirection(cli);
            if (red == ESP_OK) {
                esp_http_client_get_url(cli, next, sizeof(next));
            }
            esp_http_client_cleanup(cli);
            if (red != ESP_OK || !next[0]) {
                return ESP_ERR_NOT_FOUND;
            }
            ESP_LOGI(TAG, "station list redirect %d -> %.60s", code, next);
            strlcpy(url, next, sizeof(url));
            continue;
        }

        /* ⚠️ 这里必须用 sizeof(s_http_body)，不能用 sizeof(body)：
           body 是 char* 指针，sizeof(body) 恒等于 4（指针大小），
           于是循环只会读进 3 个字节就退出 -> JSON 被截断 -> 解析不到任何台。
           这就是「电台读不到列表」的根因。 */
        n = 0;
        int r;
        while (n < (int)cap - 1 &&
               (r = esp_http_client_read(cli, body + n, cap - 1 - n)) > 0) {
            n += r;
        }
        esp_http_client_cleanup(cli);
        body[n] = '\0';
        break;
    }

    if (code != 200) {
        ESP_LOGW(TAG, "station list http %d (%d bytes)", code, n);
        return ESP_ERR_NOT_FOUND;
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "station list empty (http %d)", code);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "station list: %d bytes, http %d (%s)", n, code, url);

    int cnt = 0;
    esp_err_t e = parse_stations(body, s_stage, &cnt);
    if (e != ESP_OK || cnt <= 0) {
        return e;
    }
    /* 解析成功才替换：s_cnt 全程不会出现中间态 0 */
    memcpy(s_st, s_stage, (size_t)cnt * sizeof(s_st[0]));
    s_cnt = cnt;
    if (s_cur >= s_cnt) {
        s_cur = 0;
    }
    return ESP_OK;
}

/** 依次尝试所有镜像源，任一成功即可 */
static esp_err_t radio_fetch_list(void)
{
    esp_err_t err = ESP_FAIL;
    for (size_t i = 0; i < RADIO_LIST_URL_CNT; i++) {
        err = radio_fetch_list_one(s_radio_list_urls[i]);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "list source %u failed (%s), trying next",
                 (unsigned)i, esp_err_to_name(err));
    }
    return err;
}

/* ==================== 播放源（MP3 直连 / HLS） ==================== */

typedef struct {
    esp_http_client_handle_t cli;
    bool hls;
    char base[RADIO_URL_MAX];              /* 当前 media playlist URL（相对地址基准） */
    char segs[8][RADIO_URL_MAX];
    int seg_cnt, seg_pos;
    bool need_playlist;
} src_t;

static void src_close_cli(src_t *s)
{
    if (s->cli) {
        esp_http_client_cleanup(s->cli);
        s->cli = NULL;
    }
}

/**
 * 相对地址 -> 绝对地址（RFC 3986 简化版）。
 *
 * 老实现直接 strrchr(base,'/') 截断，两个坑：
 *   1. base 带 query（.../index.m3u8?token=abc/123）时，最后一个 '/' 落在
 *      参数里面，拼出来的分段地址完全非法；
 *   2. rel 是根路径（/live/seg1.ts）时会拼成 http://host/dir//live/seg1.ts。
 * 现在先剥离 base 的 query/fragment，再按「绝对 / 协议相对 / 根路径 / 同目录」
 * 分别处理，并吃掉开头的 ./ 与 ../。
 */
static void resolve_url(const char *base, const char *rel, char *out, int outlen)
{
    if (strncasecmp(rel, "http://", 7) == 0 || strncasecmp(rel, "https://", 8) == 0) {
        strlcpy(out, rel, outlen);
        return;
    }

    /* scheme://authority 前缀 */
    char prefix[160] = {0};
    const char *path = "";
    size_t plen = 0;
    const char *auth = strstr(base, "://");
    if (auth) {
        auth += 3;
        const char *slash = strchr(auth, '/');
        size_t n = slash ? (size_t)(slash - base) : strlen(base);
        if (n >= sizeof(prefix)) {
            n = sizeof(prefix) - 1;
        }
        memcpy(prefix, base, n);
        prefix[n] = '\0';
        if (slash) {
            path = slash;
            plen = strlen(slash);
        } else {
            path = "/";
            plen = 1;
        }
    } else {
        strlcpy(prefix, base, sizeof(prefix));
        path = base;
        plen = strlen(base);
    }

    /* 协议相对：//host/path */
    if (rel[0] == '/' && rel[1] == '/') {
        const char *c = strstr(prefix, "://");
        char sch[8] = "http";
        if (c) {
            size_t n = (size_t)(c - prefix);
            if (n >= sizeof(sch)) {
                n = sizeof(sch) - 1;
            }
            memcpy(sch, prefix, n);
            sch[n] = '\0';
        }
        snprintf(out, (size_t)outlen, "%s:%s", sch, rel);
        return;
    }

    /* base 的 path 去掉 query / fragment */
    char dir[RADIO_URL_MAX];
    size_t keep = 0;
    for (size_t i = 0; i < plen && i < sizeof(dir) - 1; i++) {
        if (path[i] == '?' || path[i] == '#') {
            break;
        }
        dir[i] = path[i];
        keep = i + 1;
    }
    dir[keep] = '\0';

    if (rel[0] == '/') {
        /* 根路径相对：直接用 authority + rel */
        snprintf(out, (size_t)outlen, "%s%s", prefix, rel);
        return;
    }

    /* 同目录相对：保留到最后一个 '/'（含） */
    char *slash = strrchr(dir, '/');
    if (slash) {
        slash[1] = '\0';
    } else {
        strlcpy(dir, "/", sizeof(dir));
    }

    const char *r = rel;
    for (;;) {
        if (r[0] == '.' && r[1] == '/') {
            r += 2;
            continue;
        }
        if (r[0] == '.' && r[1] == '.' && r[2] == '/') {
            r += 3;
            /* dir 现在形如 "/a/b/"：先去掉尾斜杠，再砍掉最后一段 */
            size_t dl = strlen(dir);
            if (dl > 1) {
                if (dir[dl - 1] == '/') {
                    dir[--dl] = '\0';
                }
                char *prev = strrchr(dir, '/');
                if (prev) {
                    prev[1] = '\0';
                } else {
                    strlcpy(dir, "/", sizeof(dir));
                }
            }
            continue;
        }
        break;
    }
    snprintf(out, (size_t)outlen, "%s%s%s", prefix, dir, r);
}

/**
 * 打开一个流式 HTTP 连接：跟随 301/302/303/307/308，且只接受 200。
 *
 * ⚠️ 手动流模式（open -> fetch_headers -> read）下 esp_http_client
 * **不会**自动跟随重定向，也不会替我们判状态码。蜻蜓/CDN 经常先回 302，
 * 老代码把 302 的 HTML 错误页当 MP3 数据喂给解码器，播两秒就报
 * 「直播中断，连接断开」。
 *
 * 注意：Location 在响应解析阶段已存进 client->location，
 * 用 set_redirection + get_url 取（响应头默认不保存，
 * esp_http_client_get_response_header 需开 SAVE_RESPONSE_HEADERS）。
 */
static esp_err_t stream_open(src_t *s, const char *url, char *final, size_t final_len)
{
    char cur[512];
    strlcpy(cur, url, sizeof(cur));

    for (int hop = 0; hop < 5; hop++) {
        esp_http_client_config_t cfg = {
            .url = cur,
            /* 首跳是 DNS+TLS 握手，3s 太紧（实测蜻蜓握手 1.5~2.5s） */
            .timeout_ms = 6000,
            .buffer_size = 4096,
            .keep_alive_enable = false,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        s->cli = esp_http_client_init(&cfg);
        if (!s->cli) {
            return ESP_FAIL;
        }
        if (esp_http_client_open(s->cli, 0) != ESP_OK) {
            src_close_cli(s);
            return ESP_FAIL;
        }
        esp_http_client_fetch_headers(s->cli);
        int code = esp_http_client_get_status_code(s->cli);

        if (code == 200) {
            if (final && final_len) {
                strlcpy(final, cur, final_len);
            }
            return ESP_OK;
        }

        bool redirect = (code == 301 || code == 302 || code == 303 ||
                         code == 307 || code == 308);
        if (!redirect) {
            ESP_LOGW(TAG, "stream http %d -> %.70s", code, cur);
            src_close_cli(s);
            return ESP_FAIL;
        }

        char next[512] = {0};
        esp_err_t red = esp_http_client_set_redirection(s->cli);
        if (red == ESP_OK) {
            esp_http_client_get_url(s->cli, next, sizeof(next));
        }
        src_close_cli(s);
        if (red != ESP_OK || !next[0]) {
            return ESP_FAIL;
        }
        strlcpy(cur, next, sizeof(cur));
        ESP_LOGI(TAG, "stream redirect %d -> %.70s", code, cur);
    }
    return ESP_FAIL;
}

/** 拉 playlist 文本。返回 0=media playlist, 1=master(redirect 填入), -1=失败 */
static int fetch_playlist(const char *url, char *segs, int max_segs, int *seg_cnt,
                          char *redirect)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) {
        return -1;
    }
    if (esp_http_client_open(cli, 0) != ESP_OK) {
        esp_http_client_cleanup(cli);
        return -1;
    }
    esp_http_client_fetch_headers(cli);
    int code = esp_http_client_get_status_code(cli);
    if (code != 200) {
        /* 302/403 的响应体是 HTML 错误页，当 playlist 解析只会得到
           一堆垃圾分段地址，不如直接判失败换源 */
        ESP_LOGW(TAG, "playlist http %d", code);
        esp_http_client_cleanup(cli);
        return -1;
    }

    /* 用独立缓冲：电台列表刷新任务可能同时在写 s_http_body */
    char *text = s_pl_body;
    const size_t cap = sizeof(s_pl_body);
    int n = 0, r;
    while (n < (int)cap - 1 &&
           (r = esp_http_client_read(cli, text + n, cap - 1 - n)) > 0) {
        n += r;
    }
    text[n] = '\0';
    esp_http_client_cleanup(cli);
    if (n <= 0) {
        return -1;
    }

    *seg_cnt = 0;
    bool want_stream = false;
    char *save = NULL;
    for (char *line = strtok_r(text, "\r\n", &save); line;
         line = strtok_r(NULL, "\r\n", &save)) {
        if (line[0] == '#') {
            want_stream = (strncmp(line, "#EXT-X-STREAM-INF", 17) == 0);
            continue;
        }
        if (line[0] == '\0') {
            continue;
        }
        if (want_stream) {
            /* master playlist：取第一个流地址再去拉 media playlist */
            resolve_url(url, line, redirect, RADIO_URL_MAX);
            return 1;
        }
        if (*seg_cnt < max_segs) {
            char abs[RADIO_URL_MAX];
            resolve_url(url, line, abs, sizeof(abs));
            if (!was_played(abs)) {
                strlcpy(segs + *seg_cnt * RADIO_URL_MAX, abs, RADIO_URL_MAX);
                (*seg_cnt)++;
            }
        }
    }
    return (*seg_cnt > 0) ? 0 : -1;
}

static esp_err_t src_open(src_t *s, const char *url)
{
    memset(s, 0, sizeof(*s));
    s->hls = (strstr(url, ".m3u8") != NULL);
    strlcpy(s->base, url, sizeof(s->base));
    if (s->hls) {
        /* HLS： playlist 稍后按需解析 */
        s->need_playlist = true;
        return ESP_OK;
    }
    char final[RADIO_URL_MAX];
    esp_err_t e = stream_open(s, url, final, sizeof(final));
    if (e != ESP_OK) {
        return e;
    }
    strlcpy(s->base, final, sizeof(s->base));
    return ESP_OK;
}

/** HLS：解析 playlist 并打开下一个分段 */
static esp_err_t src_hls_next_segment(src_t *s)
{
    int redirect_guard = 0;
    while (s->need_playlist) {
        char redirect[RADIO_URL_MAX] = {0};
        int rc = fetch_playlist(s->base, (char *)s->segs, 8, &s->seg_cnt, redirect);
        if (rc == 1) {
            strlcpy(s->base, redirect, sizeof(s->base));   /* master -> media */
            if (++redirect_guard > 3) {
                return ESP_FAIL;
            }
            continue;
        }
        if (rc != 0) {
            return ESP_FAIL;
        }
        s->need_playlist = false;
        s->seg_pos = 0;
        if (s->seg_cnt == 0) {
            /* 全部播过（追到直播边缘），等下一轮 */
            set_status("直播等待中…");
            vTaskDelay(pdMS_TO_TICKS(2000));
            s->need_playlist = true;
            return ESP_ERR_NOT_FINISHED;
        }
    }

    const char *seg = s->segs[s->seg_pos++];
    mark_played(seg);
    ESP_LOGI(TAG, "hls segment %d/%d", s->seg_pos, s->seg_cnt);

    /* 分段同样可能 302（CDN 节点）且必须校验状态码：
       原来 1000ms 超时 + 不判状态码，HTTPS 分段几乎必然失败。 */
    if (stream_open(s, seg, NULL, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ==================== 播放任务 ==================== */

/** 从当前源读数据进线性缓冲。>0=读了 n 字节，0=暂时没数据，<0=源错误 */
static int src_fill(src_t *s)
{
    if (!s->cli) {
        if (!s->hls) {
            return -1;
        }
        esp_err_t e = src_hls_next_segment(s);
        if (e == ESP_ERR_NOT_FINISHED) {
            return 0;
        }
        if (e != ESP_OK) {
            return -1;
        }
    }
    if (s_len >= HIGH_WATER) {
        return 0;
    }
    int want = (BUF_CAP - s_len) > READ_CHUNK ? READ_CHUNK : (BUF_CAP - s_len);
    int n = esp_http_client_read(s->cli, (char *)s_buf + s_len, want);
    if (n > 0) {
        s_len += n;
        return n;
    }
    if (n == 0) {
        src_close_cli(s);
        if (s->hls) {
            s->need_playlist = true;   /* 分段播完，取下一段 */
        }
        return 0;
    }
    return -1;
}

/** 播放结果 */
typedef enum {
    PLAY_STOPPED,    /* 用户停止 */
    PLAY_SWITCHED,   /* 换台，需要重播新台 */
    PLAY_ERROR,      /* 连接/解码失败 */
} play_result_t;

/* 解码失败时最多允许跳过多少字节的垃圾再判定「不是 MP3」。
   64KB 足够吃掉 ID3 标签 / TS 包头 / ICY 头；真遇到 AAC 流
   （ADTS 也是 0xFFF? 同步字，但帧头解析必失败）会很快超限。 */
#define SKIP_LIMIT  (64 * 1024)

/** 在缓冲里找 MP3 帧同步字（0xFF 后跟 3 个 1，即 0xE0 掩码），返回偏移或 -1 */
static int find_sync(const uint8_t *b, int n)
{
    for (int i = 0; i + 1 < n; i++) {
        if (b[i] == 0xFF && (b[i + 1] & 0xE0) == 0xE0) {
            return i;
        }
    }
    return -1;
}

static play_result_t play_station(int idx)
{
    src_t src;
    char title[40];

    strlcpy(title, s_st[idx].name, sizeof(title));
    ui_show_action(title, "连接直播源…");
    set_status("连接中…");
    s_len = 0;
    memset(s_played, 0, sizeof(s_played));

    if (src_open(&src, s_st[idx].url) != ESP_OK) {
        set_status("连接失败");
        ui_show_action(title, "连接失败，试试其他台");
        return PLAY_ERROR;
    }

    s_dec = MP3InitDecoder();
    if (!s_dec) {
        src_close_cli(&src);
        set_status("解码器内存不足");
        return PLAY_ERROR;
    }

    bool audio_started = false;
    int empty_runs = 0;
    int skipped = 0;
    int write_fails = 0;
    play_result_t result = PLAY_STOPPED;
    set_status("缓冲中…");

    while (true) {
        /* 帧边界处理请求（换台/停止最迟约 26ms 响应） */
        if (s_req == REQ_TOGGLE) {
            s_req = REQ_NONE;
            result = PLAY_STOPPED;
            break;
        }
        /* 电台列表点选：切到新台继续播，不能当成「停止」。
           以前两者合在一个分支里且都置 PLAY_STOPPED，点一下列表就停播，
           而 radio_task 里那句 `if (s_req == REQ_SELECT) continue;`
           因为 s_req 已被清成 REQ_NONE 永远不成立。 */
        if (s_req == REQ_SELECT) {
            s_req = REQ_NONE;
            show_station("切换中…");
            result = PLAY_SWITCHED;
            break;
        }
        if (s_req == REQ_NEXT || s_req == REQ_PREV) {
            if (s_cnt <= 0) {        /* 列表被换空了也不可能除零 */
                s_req = REQ_NONE;
                continue;
            }
            s_cur = (s_cur + (s_req == REQ_NEXT ? 1 : s_cnt - 1)) % s_cnt;
            s_req = REQ_NONE;
            show_station("切换中…");
            result = PLAY_SWITCHED;
            break;
        }

        /* 数据不足先攒 */
        if (s_len < DECODE_MIN) {
            int n = src_fill(&src);
            if (n < 0) {
                set_status("直播中断");
                ui_show_action(title, "连接断开，点播放重试");
                result = PLAY_ERROR;
                break;
            }
            if (n == 0 && ++empty_runs > 30) {   /* ~30*300ms 无数据 */
                set_status("直播中断");
                ui_show_action(title, "连接断开，点播放重试");
                result = PLAY_ERROR;
                break;
            }
            if (n == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue;
        }
        empty_runs = 0;

        /* 顺手再攒一点（读函数内部是短超时，最多阻塞 ~1s） */
        if (s_len < HIGH_WATER) {
            (void)src_fill(&src);
        }

        /* 解码一帧 */
        uint8_t *in = s_buf;
        int left = s_len;
        int err = MP3Decode(s_dec, &in, &left, s_pcm, 0);
        int consumed = s_len - left;
        if (err != ERR_MP3_NONE) {
            if (consumed > 0) {
                /* 解码器自己跳过了若干字节（前导垃圾 / 半截帧） */
                memmove(s_buf, s_buf + consumed, s_len - consumed);
                s_len -= consumed;
            } else {
                /* ⚠️ 必须丢**头部**字节。老代码写的是 s_len--（丢尾部），
                   s_buf[0] 始终不变 -> MP3Decode 在同一位置反复报同一个
                   错误，空转 4000 次后误报「流不是 MP3 编码」。
                   这里一次跳到下一个候选同步字，ID3 标签 / TS 包头几百
                   字节内就能跳过，而不是一字节一字节地磨。 */
                int off = (s_len > 1) ? find_sync(s_buf + 1, s_len - 1) : -1;
                int drop = (off >= 0) ? off + 1 : (s_len > 1 ? s_len - 1 : s_len);
                if (drop < 1) {
                    drop = 1;
                }
                if (drop > s_len) {
                    drop = s_len;
                }
                memmove(s_buf, s_buf + drop, s_len - drop);
                s_len -= drop;
                skipped += drop;
            }
            /* 只有「一直没解出任何一帧」才判死；已经在播时的零星
               同步错误（TS 包头等）直接跳过，不能因此停台。 */
            if (!audio_started && skipped > SKIP_LIMIT) {
                set_status("解码失败");
                ui_show_action(title, "流不是 MP3 编码，试试其他台");
                result = PLAY_ERROR;
                break;
            }
            continue;
        }
        skipped = 0;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(s_dec, &fi);
        if (!audio_started && fi.samprate > 0) {
            /* 启动失败必须停台报错：老代码忽略返回值继续解码，
               bsp_audio_write 恒失败 —— 界面显示「播放中」但全程无声。 */
            if (bsp_audio_start(fi.samprate) != ESP_OK) {
                set_status("音频启动失败");
                ui_show_action(title, "音频硬件不可用");
                result = PLAY_ERROR;
                break;
            }
            audio_started = true;
            char sub[48];
            snprintf(sub, sizeof(sub), "播放中 %dHz %s",
                     fi.samprate, fi.nChans == 2 ? "立体声" : "单声道");
            set_status(sub);
            ui_show_action(title, sub);
        }

        int chans = fi.nChans;
        int frames = fi.outputSamps / chans;
        if (chans == 1) {
            /* 单声道复制成双声道 */
            for (int i = frames - 1; i >= 0; i--) {
                s_pcm[2 * i] = s_pcm[i];
                s_pcm[2 * i + 1] = s_pcm[i];
            }
        }
        /* 连续写失败（音频通道没起来）要停台，不能假装在播 */
        if (bsp_audio_write(s_pcm, frames * 2) != ESP_OK) {
            if (++write_fails > 8) {
                set_status("音频输出失败");
                ui_show_action(title, "音频输出异常，请重试");
                result = PLAY_ERROR;
                break;
            }
        } else {
            write_fails = 0;
        }

        if (consumed > 0) {
            memmove(s_buf, s_buf + consumed, s_len - consumed);
            s_len -= consumed;
        }
    }

    MP3FreeDecoder(s_dec);
    s_dec = NULL;
    src_close_cli(&src);
    bsp_audio_stop();
    return result;
}

/* 前向声明：radio_task 里列表未就绪时自动重拉 */
static void fetch_list_task(void *arg);
static BaseType_t radio_task_create(TaskFunction_t fn, const char *name,
                                   uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out);

static void radio_task(void *arg)
{
    (void)arg;
    for (;;) {
        radio_req_t req = s_req;
        if (req == REQ_NONE) {
            vTaskDelay(pdMS_TO_TICKS(80));
            continue;
        }
        s_req = REQ_NONE;

        if (req == REQ_NEXT || req == REQ_PREV) {
            if (!s_cnt) {
                continue;
            }
            s_cur = (s_cur + (req == REQ_NEXT ? 1 : s_cnt - 1)) % s_cnt;
            show_station("待播放");
            continue;
        }

        /* REQ_TOGGLE */
        if (s_cnt <= 0) {
            /* 台列表还没拿到（首次拉取失败 / WiFi 刚起来）：
               立刻用内置台兜底，让「按下就有声」，同时后台继续拉在线列表，
               拉到了再无缝替换。用户不再需要「重进电台页」。 */
            seed_defaults();
            if (!s_list_loading) {
                radio_task_create(fetch_list_task, "radio_list", RADIO_LIST_STACK, NULL, 3, NULL);
            }
            if (s_cnt <= 0) {
                show_station("没有可用的台");
                continue;
            }
        }

        /* ⚠️ 播放前先确保 WiFi 在线（幂等，已连则立即返回）：
           没网时 MP3 流必然「连接失败」，先建网再播，状态条同步显示进度 */
        if (!ota_update_wifi_connected()) {
            set_status("连接 WiFi…");
            show_station("先连 WiFi，连上就播");
            if (ota_update_wifi_online() != ESP_OK) {
                set_status("WiFi 未连上");
                show_station("连不上 WiFi，检查热点后重试");
                continue;
            }
        }

        /* 播放循环：换台自动接续，直到用户停止或出错 */
        for (;;) {
            play_result_t r = play_station(s_cur);
            if (r != PLAY_SWITCHED) {
                break;
            }
        }
        /* 选台的常规路径已在 play_station 内返回 PLAY_SWITCHED 由上面的
           循环接续；这里只兜住「刚好在 play_station 返回后才按到选台」的
           竞态：否则会被下面的收尾逻辑改写成「已停止」。 */
        if (s_req == REQ_SELECT) {
            continue;
        }
        if (s_status[0] && strcmp(s_status, "已停止") != 0 &&
            strncmp(s_status, "直播中断", 4) != 0 && strcmp(s_status, "解码失败") != 0) {
            set_status("已停止");
            show_station("已停止");
        }
    }
}

/* ==================== 对外接口 ==================== */

/**
 * 电台任务的栈统一放 PSRAM。
 *
 * WiFi(~45KB)+蓝牙(~60KB)+LCD DMA 私有缓冲全在场后，内部 RAM 只剩
 * 几 KB（最大块 4KB），8K/10K 的内部栈根本分不出来 —— 表现为
 * 「radio buffers alloc failed」「列表永远拉不到」「点了没声」。
 * 这两个任务只做 HTTP/TLS + MP3 解码 + I2S 写，不碰 flash 写操作，
 * 栈放 PSRAM 完全安全；退出时 xTaskCreateWithCaps 配对的
 * vTaskDeleteWithCaps 会把 PSRAM 栈回收。
 */
static inline BaseType_t radio_task_create(TaskFunction_t fn, const char *name,
                                           uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out)
{
    return xTaskCreateWithCaps(fn, name, stack, arg, prio, out,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void ensure_task(void)
{
    if (!s_task) {
        radio_task_create(radio_task, "radio", 8192, NULL, 4, &s_task);
    }
}

static void fetch_list_task(void *arg)
{
    (void)arg;
    s_list_loading = true;
    show_station("连接 WiFi…");
    set_status("连接 WiFi…");

    esp_err_t e = ota_update_wifi_online();
    if (e != ESP_OK) {
        set_status("WiFi 未连上");
        ui_show_action("电台", "WiFi 连接失败");
        s_list_loading = false;
        vTaskDeleteWithCaps(NULL);
        return;
    }

    set_status("拉取台列表…");
    e = radio_fetch_list();
    s_list_loading = false;
    if (e == ESP_OK) {
        char sub[48];
        snprintf(sub, sizeof(sub), "共 %d 个台，点播放收听", s_cnt);
        set_status(sub);
        show_station(sub);
    } else {
        /* 在线列表失败也不让电台变砖：内置台继续可用 */
        seed_defaults();
        set_status("在线列表失败，用内置台");
        show_station("在线列表获取失败，内置台可直接播");
    }
    vTaskDeleteWithCaps(NULL);
}

void radio_enter(void)
{
    ensure_task();
    if (!s_buf) {
        s_buf = heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        /* PCM 只是 i2s_channel_write 的**源**缓冲，I2S 驱动会把它拷进自己
           的内部 DMA 缓冲，所以这块不需要内部 RAM。放 PSRAM 可以省下
           4.6KB 内部 RAM —— 内部最大块只剩 4KB 时（WiFi+蓝牙+LCD DMA
           私有缓冲全在场），放内部会直接分配失败：radio buffers alloc failed。 */
        s_pcm = heap_caps_malloc(1152 * 2 * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_pcm) {   /* PSRAM 异常时退回内部，能用总比不能用强 */
            s_pcm = heap_caps_malloc(1152 * 2 * sizeof(int16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        }
        if (!s_buf || !s_pcm) {
            ESP_LOGE(TAG, "radio buffers alloc failed");
            return;
        }
        esp_err_t e = bsp_audio_init();
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "audio init failed (%s), playback unavailable",
                     esp_err_to_name(e));
            set_status("音频硬件不可用");
        }
    }
    /* 内置台先兜底：一进电台页就有台可播，不用等网络往返 */
    if (s_cnt == 0) {
        seed_defaults();
        set_status("内置台，正在拉在线列表…");
    }
    /* 每次都刷新一次在线列表（拿到就替换内置台，拿不到保留内置台） */
    if (!s_list_loading) {
        radio_task_create(fetch_list_task, "radio_list", RADIO_LIST_STACK, NULL, 3, NULL);
    }
    show_station(s_status[0] ? s_status : "待播放");
}

void radio_next(void)
{
    if (!s_cnt) {
        return;
    }
    s_req = REQ_NEXT;
}

void radio_prev(void)
{
    if (!s_cnt) {
        return;
    }
    s_req = REQ_PREV;
}

void radio_toggle(void)
{
    ensure_task();
    s_req = REQ_TOGGLE;
}

void radio_select(int idx)
{
    if (idx < 0 || idx >= s_cnt) {
        return;
    }
    ensure_task();
    s_cur = idx;
    s_req = REQ_SELECT;
    show_station("准备播放");
}

void radio_stop(void)
{
    /* 用 REQ_TOGGLE 只在播放中生效；空闲时无害 */
    if (strstr(s_status, "播放中") || strstr(s_status, "缓冲中") ||
        strstr(s_status, "直播等待")) {
        s_req = REQ_TOGGLE;
    } else {
        s_req = REQ_NONE;
    }
}

void radio_volume(int vol)
{
    bsp_audio_volume_set(vol);
    char sub[24];
    snprintf(sub, sizeof(sub), "电台音量 %d%%", vol);
    ui_set_footer(sub);
}

/* ==================== 串口命令 ==================== */

static int cmd_radio(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "list") == 0) {
        if (!s_cnt) {
            printf("station list empty, run 'radio update'\n");
            return 0;
        }
        for (int i = 0; i < s_cnt; i++) {
            printf("[%d] %s %s\n    %s\n", i,
                   i == s_cur ? "->" : "  ", s_st[i].name, s_st[i].url);
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "play") == 0) {
        int idx = atoi(argv[2]);
        if (idx < 0 || idx >= s_cnt) {
            printf("error: index 0..%d\n", s_cnt - 1);
            return 1;
        }
        s_cur = idx;
        s_req = REQ_TOGGLE;
        printf("playing [%d] %s\n", idx, s_st[idx].name);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "stop") == 0) {
        s_req = REQ_TOGGLE;
        printf("stop requested\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "vol") == 0) {
        radio_volume(atoi(argv[2]));
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "update") == 0) {
        /* 不要把 s_cnt 清零：清 0 期间正在播放的任务会算 s_cur % 0。
           新列表解析成功后整体替换，失败则保留现有（内置）台。 */
        if (!s_list_loading) {
            radio_task_create(fetch_list_task, "radio_list", RADIO_LIST_STACK, NULL, 3, NULL);
        }
        printf("refreshing station list...\n");
        return 0;
    }
    printf("usage: radio list | radio play <n> | radio stop | radio vol <0-100> | radio update\n");
    return 1;
}

void radio_register_cli(void)
{
    esp_console_cmd_t cmd = {
        .command = "radio",
        .help = "WiFi radio: list/play/stop/vol/update",
        .hint = "list | play <n> | stop | vol <0-100> | update",
        .func = cmd_radio,
    };
    esp_console_cmd_register(&cmd);
}
