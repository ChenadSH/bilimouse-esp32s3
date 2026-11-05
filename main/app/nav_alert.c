/**
 * @file nav_alert.c
 * @brief 导航提醒实现：HTTP 接收 -> 屏幕 + 提示音
 *
 * 并发说明：httpd 回调运行在 httpd 任务里，这里只更新一段小缓冲 +
 * 标脏 UI + 触发提示音任务，绝不做阻塞操作。
 */
#include "nav_alert.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_audio.h"
#include "ui.h"

static const char *TAG = "nav";

static char s_text[NAV_TEXT_MAX];
static int64_t s_ts;            /* us since boot */
static httpd_handle_t s_server;

/* ==================== 提示音 ==================== */

/** 两声「叮咚」：880Hz + 1318Hz，各 150ms，44100Hz 16bit 双声道。
    提示音按 512 样本分块生成，避免再占一块 26KB 的静态内部 RAM
    （v2.5 首版就是这么写的，直接把 WiFi 挤到 NO_MEM）。
    电台播放中（I2S 被占用）时跳过，绝不打断收音。 */
static void nav_beep(void)
{
    if (bsp_audio_busy()) {
        return;   /* I2S 正在播电台，别抢 */
    }
    if (bsp_audio_init() != ESP_OK || bsp_audio_start(44100) != ESP_OK) {
        return;
    }

    enum { CHUNK = 512, TOTAL = 44100 / 1000 * 150 };
    static int16_t pcm[CHUNK * 2] __attribute__((aligned(4)));
    const int freqs[2] = { 880, 1318 };
    for (int tone = 0; tone < 2; tone++) {
        for (int done = 0; done < TOTAL; done += CHUNK) {
            int n = (TOTAL - done) < CHUNK ? (TOTAL - done) : CHUNK;
            for (int i = 0; i < n; i++) {
                int k = done + i;
                /* 正弦 + 首尾 10ms 线性淡入淡出，避免爆音 */
                float ph = 2.0f * 3.14159265f * freqs[tone] * k / 44100.0f;
                float env = 1.0f;
                int edge = TOTAL / 15;
                if (k < edge) {
                    env = (float)k / edge;
                } else if (k > TOTAL - edge) {
                    env = (float)(TOTAL - k) / edge;
                }
                int16_t v = (int16_t)(18000 * env * sinf(ph));
                pcm[2 * i] = v;
                pcm[2 * i + 1] = v;
            }
            bsp_audio_write(pcm, n * 2);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(50));   /* 让最后一段进 DMA */
    bsp_audio_stop();
}

static void beep_task(void *arg)
{
    (void)arg;
    nav_beep();
    vTaskDelete(NULL);
}

/* ==================== 显示 ==================== */

static void show_nav_text(const char *text)
{
    int64_t now = esp_timer_get_time();
    strlcpy(s_text, text, sizeof(s_text));
    s_ts = now;

    ui_show_action("导航提醒", s_text);
    if (s_server) {
        ui_set_footer("手机快捷指令已推送");
    }
}

void nav_alert_push(const char *text)
{
    if (!text || !text[0]) {
        return;
    }
    show_nav_text(text);
    ESP_LOGI(TAG, "nav: %s", text);
    xTaskCreate(beep_task, "nav_beep", 3072, NULL, 3, NULL);
}

/* ==================== HTTP ==================== */

/** %XX 与 '+' 解码（UTF-8 原样透传） */
static void url_decode(const char *src, char *dst, int dstlen)
{
    int o = 0;
    while (*src && o < dstlen - 1) {
        if (src[0] == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (src[0] == '+') {
            dst[o++] = ' ';
            src++;
        } else {
            dst[o++] = *src++;
        }
    }
    dst[o] = '\0';
}

static esp_err_t nav_handler(httpd_req_t *req)
{
    char raw[128] = {0};
    char text[NAV_TEXT_MAX];

    if (req->method == HTTP_POST) {
        int n = httpd_req_recv(req, raw, sizeof(raw) - 1);
        if (n < 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_FAIL;
        }
        raw[n] = '\0';
        /* POST body 允许是 text=xxx 或纯文本 */
        if (strncmp(raw, "text=", 5) == 0) {
            url_decode(raw + 5, text, sizeof(text));
        } else {
            strlcpy(text, raw, sizeof(text));
        }
    } else {
        size_t qlen = httpd_req_get_url_query_len(req) + 1;
        if (qlen > 1 && qlen <= sizeof(raw)) {
            httpd_req_get_url_query_str(req, raw, qlen);
            char val[128];
            if (httpd_query_key_value(raw, "text", val, sizeof(val)) == ESP_OK) {
                url_decode(val, text, sizeof(text));
            } else {
                text[0] = '\0';
            }
        } else {
            text[0] = '\0';
        }
    }

    if (!text[0]) {
        httpd_resp_send(req, "BiliMouse nav ok. usage: /nav?text=<提示内容>\n",
                        HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    nav_alert_push(text);
    httpd_resp_send(req, "ok\n", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ip[16] = "0.0.0.0";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK) {
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        }
    }
    char buf[160];
    snprintf(buf, sizeof(buf), "BiliMouse nav server\nlast: %.64s\n",
             s_text[0] ? s_text : "(none)");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "status: ip=%s", ip);
    return ESP_OK;
}

esp_err_t nav_alert_init(void)
{
    if (s_server) {
        return ESP_OK;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 8080;
    cfg.lru_purge_enable = true;   /* 内存紧张时踢掉空闲连接 */
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "http server start failed (导航提醒不可用)");
        return ESP_FAIL;
    }
    httpd_uri_t nav_uri = { .uri = "/nav", .method = HTTP_GET, .handler = nav_handler };
    httpd_register_uri_handler(s_server, &nav_uri);
    nav_uri.method = HTTP_POST;
    httpd_register_uri_handler(s_server, &nav_uri);
    httpd_uri_t st_uri = { .uri = "/", .method = HTTP_GET, .handler = status_handler };
    httpd_register_uri_handler(s_server, &st_uri);
    ESP_LOGI(TAG, "nav server on :8080 (/nav?text=...)");
    return ESP_OK;
}

void nav_alert_get_ip(char *buf, int len)
{
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t info;
    if (netif && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr) {
        snprintf(buf, len, IPSTR, IP2STR(&info.ip));
    } else {
        strlcpy(buf, "未连接", len);
    }
}

bool nav_alert_has_message(void)
{
    return s_text[0] != '\0';
}

void nav_alert_get(char *buf, int len, int64_t *out_ts)
{
    strlcpy(buf, s_text, len);
    if (out_ts) {
        *out_ts = s_ts;
    }
}
