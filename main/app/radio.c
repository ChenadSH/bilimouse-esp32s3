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
static int s_cnt;
static int s_cur;
static char s_status[48];

/* 拉取 HTTP 文本的复用缓冲（台列表 ~2KB、playlist ~4KB） */
static char s_http_body[8192] EXT_RAM_BSS_ATTR;

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

/** 把 JSON 里的 \" \/ 还原成普通字符 */
static void json_unescape(char *s)
{
    char *w = s;
    while (*s) {
        if (s[0] == '\\' && (s[1] == '"' || s[1] == '/')) {
            s++;
        }
        *w++ = *s++;
    }
    *w = '\0';
}

/** 提取 "name"（不会误匹配 "name_en"）与 "stream_url" 字段对 */
static esp_err_t parse_stations(char *body)
{
    s_cnt = 0;
    const char *p = body;
    while (s_cnt < RADIO_MAX_STATIONS && (p = strstr(p, "\"name\""))) {
        const char *n = strchr(p + 6, '"');
        if (!n) break;
        n++;
        const char *ne = strchr(n, '"');
        if (!ne) break;
        const char *u = strstr(ne, "\"stream_url\"");
        if (!u) break;
        u = strchr(u + 12, '"');
        if (!u) break;
        u++;
        const char *ue = strchr(u, '"');
        if (!ue) break;

        int nl = ne - n, ul = ue - u;
        if (nl > 0 && ul > 0 && nl < (int)sizeof(s_st[s_cnt].name) &&
            ul < (int)sizeof(s_st[s_cnt].url)) {
            memcpy(s_st[s_cnt].name, n, nl);
            s_st[s_cnt].name[nl] = '\0';
            memcpy(s_st[s_cnt].url, u, ul);
            s_st[s_cnt].url[ul] = '\0';
            json_unescape(s_st[s_cnt].url);
            ESP_LOGI(TAG, "station[%d] %s | %s", s_cnt, s_st[s_cnt].name, s_st[s_cnt].url);
            s_cnt++;
        }
        p = ue;
    }
    return s_cnt ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static esp_err_t radio_fetch_list_one(const char *url)
{
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
    int code = esp_http_client_get_status_code(cli);

    /* ⚠️ 这里必须用 sizeof(s_http_body)，不能用 sizeof(body)：
       body 是 char* 指针，sizeof(body) 恒等于 4（指针大小），
       于是循环只会读进 3 个字节就退出 -> JSON 被截断 -> 解析不到任何台。
       这就是「电台读不到列表」的根因。 */
    char *body = s_http_body;
    const size_t cap = sizeof(s_http_body);
    int n = 0, r;
    while (n < (int)cap - 1 &&
           (r = esp_http_client_read(cli, body + n, cap - 1 - n)) > 0) {
        n += r;
    }
    esp_http_client_cleanup(cli);
    body[n] = '\0';
    if (code != 200) {
        ESP_LOGW(TAG, "station list http %d (%d bytes)", code, n);
        return ESP_ERR_NOT_FOUND;
    }
    if (n <= 0) {
        ESP_LOGW(TAG, "station list empty (http %d)", code);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "station list: %d bytes, http %d (%s)", n, code, url);
    return parse_stations(body);
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

/** 相对地址 -> 绝对地址（只处理最常见的「同目录相对」） */
static void resolve_url(const char *base, const char *rel, char *out, int outlen)
{
    if (strncasecmp(rel, "http", 4) == 0) {
        strlcpy(out, rel, outlen);
        return;
    }
    strlcpy(out, base, outlen);
    char *slash = strrchr(out, '/');
    if (slash) {
        *slash = '\0';
    }
    strlcat(out, "/", outlen);
    strlcat(out, rel, outlen);
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

    /* 同上：指针的 sizeof 是 4，必须用 s_http_body 的真实大小 */
    char *text = s_http_body;
    const size_t cap = sizeof(s_http_body);
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
    if (!s->hls) {
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = 1000,   /* 流读取短超时，保证及时响应停播 */
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
        strlcpy(s->base, url, sizeof(s->base));
        return ESP_OK;
    }
    /* HLS： playlist 稍后按需解析 */
    strlcpy(s->base, url, sizeof(s->base));
    s->need_playlist = true;
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

    esp_http_client_config_t cfg = {
        .url = seg,
        .timeout_ms = 1000,
        .buffer_size = 4096,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    s->cli = esp_http_client_init(&cfg);
    if (!s->cli || esp_http_client_open(s->cli, 0) != ESP_OK) {
        src_close_cli(s);
        return ESP_FAIL;
    }
    esp_http_client_fetch_headers(s->cli);
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
    int empty_runs = 0, err_runs = 0;
    play_result_t result = PLAY_STOPPED;
    set_status("缓冲中…");

    while (true) {
        /* 帧边界处理请求（换台/停止最迟约 26ms 响应） */
        if (s_req == REQ_TOGGLE || s_req == REQ_SELECT) {
            s_req = REQ_NONE;
            result = PLAY_STOPPED;
            break;
        }
        if (s_req == REQ_NEXT || s_req == REQ_PREV) {
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
                memmove(s_buf, s_buf + consumed, s_len - consumed);
                s_len -= consumed;
            } else {
                s_len--;    /* 逐字节滑动找帧同步 */
            }
            if (++err_runs > 4000) {
                set_status("解码失败");
                ui_show_action(title, "这个台可能不是 MP3 编码");
                result = PLAY_ERROR;
                break;
            }
            continue;
        }
        err_runs = 0;

        MP3FrameInfo fi;
        MP3GetLastFrameInfo(s_dec, &fi);
        if (!audio_started && fi.samprate > 0) {
            bsp_audio_start(fi.samprate);
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
        bsp_audio_write(s_pcm, frames * 2);

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
        if (!s_list_loading && s_cnt == 0) {
            /* 列表没拿到（首次拉取失败也走到这）：自动重拉一次，
               不让用户被迫「重进电台页」。 */
            show_station("列表未就绪，自动重拉…");
            xTaskCreate(fetch_list_task, "radio_list", RADIO_LIST_STACK, NULL, 3, NULL);
            continue;
        }
        if (s_cnt == 0) {
            show_station("还在拉取台列表…");
            continue;
        }

        /* 播放循环：换台自动接续，直到用户停止或出错 */
        for (;;) {
            play_result_t r = play_station(s_cur);
            if (r != PLAY_SWITCHED) {
                break;
            }
        }
        /* 播放中点了台列表选台：立即重新进入播放（新台已写入 s_cur） */
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

static void ensure_task(void)
{
    if (!s_task) {
        xTaskCreate(radio_task, "radio", 8192, NULL, 4, &s_task);
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
        vTaskDelete(NULL);
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
        set_status("台列表获取失败");
        ui_show_action("电台", "列表获取失败，重进电台重试");
    }
    vTaskDelete(NULL);
}

void radio_enter(void)
{
    ensure_task();
    if (!s_buf) {
        s_buf = heap_caps_malloc(BUF_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_pcm = heap_caps_malloc(1152 * 2 * sizeof(int16_t),
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
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
    if (!s_list_loading && s_cnt == 0) {
        xTaskCreate(fetch_list_task, "radio_list", RADIO_LIST_STACK, NULL, 3, NULL);
    }
    show_station(s_cnt ? s_status : "拉取台列表中…");
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
        s_cnt = 0;
        s_cur = 0;
        xTaskCreate(fetch_list_task, "radio_list", RADIO_LIST_STACK, NULL, 3, NULL);
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
