/**
 * @file ui.c
 * @brief BiliMouse 屏幕 UI 实现
 *
 * 自研轻量渲染器：RGB565 双缓冲 + 点阵字库，
 * 不引入 LVGL 等外部 GUI 框架，保证工程零外部依赖、离线可编译。
 */
#include "ui.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_lcd.h"
#include "ui_font.h"

static const char *TAG = "ui";

#define SCR_W   320
#define SCR_H   240

/* 颜色 (RGB565) */
#define COL_BG          0x0861   /* 深蓝黑背景 */
#define COL_PANEL       0x10E3   /* 面板底色 */
#define COL_ACCENT      0x2D7F   /* 强调色（B 站蓝） */
#define COL_TEXT        0xFFFF   /* 主文字白 */
#define COL_SUBTEXT     0x8410   /* 次文字灰 */
#define COL_OK          0x07E0   /* 绿 */
#define COL_WARN        0xFFE0   /* 黄 */
#define COL_ERR         0xF800   /* 红 */

/* 布局 */
#define HEADER_H        28
#define FOOTER_Y        (SCR_H - 24)

static bsp_lcd_t s_lcd;
static uint16_t *s_buf = NULL;     /* 双缓冲 */
static bool s_dirty = true;

static ui_conn_state_t s_conn = UI_CONN_DISCONNECTED;
static ui_mode_t s_mode = UI_MODE_REMOTE;
static char s_title[32] = "BiliMouse";
static char s_subtitle[64] = "";
static char s_footer[64] = "";

/* ==================== 绘图基础 ==================== */

static inline void px_set(uint16_t *buf, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) {
        return;
    }
    buf[y * SCR_W + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            px_set(s_buf, i, j, color);
        }
    }
}

static void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    /* 简化的圆角矩形：四角各挖去一个小方块 */
    fill_rect(x + r, y, w - 2 * r, h, color);
    fill_rect(x, y + r, r, h - 2 * r, color);
    fill_rect(x + w - r, y + r, r, h - 2 * r, color);
    /* 角上补齐（用主体色近似，视觉上足够圆润） */
    fill_rect(x + r, y + r, w - 2 * r, r, color);
    fill_rect(x + r, y + h - r, w - 2 * r, r, color);
}

/**
 * 绘制一个点阵字形
 * 格式: {bpr, row[bpr*16]}，MSB-first
 */
static void draw_glyph(int x, int y, const uint8_t *glyph, int w, uint16_t color)
{
    if (!glyph) {
        return;
    }
    uint8_t bpr = glyph[0];
    const uint8_t *rows = glyph + 1;
    for (int gy = 0; gy < 16; gy++) {
        for (int gx = 0; gx < w; gx++) {
            int byte = gx / 8;
            int bit = 7 - (gx % 8);
            if (byte >= bpr) {
                continue;
            }
            if (rows[gy * bpr + byte] & (1 << bit)) {
                px_set(s_buf, x + gx, y + gy, color);
            }
        }
    }
}

/**
 * 绘制 UTF-8 字符串（中文按 UTF-8 解码为码点）
 * @param max_w 可用宽度，超出则截断
 * @return 实际绘制宽度
 */
static int draw_text(int x, int y, const char *str, uint16_t color, int max_w)
{
    if (!str) {
        return 0;
    }
    int cx = x;
    const uint8_t *p = (const uint8_t *)str;

    while (*p && cx - x < max_w) {
        uint32_t cp;
        int adv;

        if (*p < 0x80) {
            cp = *p++;
            adv = UI_FONT_ASCII_W;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = (uint32_t)(*p & 0x1F) << 6;
            p++;
            if (*p) { cp |= (*p & 0x3F); p++; }
            adv = UI_FONT_CJK_W;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = (uint32_t)(*p & 0x0F) << 12;
            p++;
            if (*p) { cp |= (uint32_t)(*p & 0x3F) << 6; p++; }
            if (*p) { cp |= (*p & 0x3F); p++; }
            adv = UI_FONT_CJK_W;
        } else {
            /* 跳过无法识别的字节 */
            p++;
            continue;
        }

        const uint8_t *glyph = (cp >= 0x20 && cp <= 0x7E)
                                   ? ui_font_get_ascii((uint8_t)cp)
                                   : ui_font_get_cjk((uint16_t)cp);
        if (!glyph) {
            adv = UI_FONT_ASCII_W;
            glyph = ui_font_get_ascii('?');
            if (!glyph) {
                continue;
            }
        }

        draw_glyph(cx, y, glyph, adv, color);
        cx += adv;
    }
    return cx - x;
}

static int text_width(const char *str)
{
    if (!str) {
        return 0;
    }
    int w = 0;
    const uint8_t *p = (const uint8_t *)str;
    while (*p) {
        if (*p < 0x80) {
            w += UI_FONT_ASCII_W;
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            w += UI_FONT_CJK_W;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            w += UI_FONT_CJK_W;
            p += 3;
        } else {
            p++;
        }
    }
    return w;
}

/* ==================== 界面元素 ==================== */

/** 顶部状态栏：连接状态点 + 模式文字 */
static void draw_header(void)
{
    fill_rect(0, 0, SCR_W, HEADER_H, COL_PANEL);

    uint16_t dot_col;
    switch (s_conn) {
    case UI_CONN_CONNECTED:    dot_col = COL_OK;    break;
    case UI_CONN_PAIRING:      dot_col = COL_WARN;  break;
    case UI_CONN_DISCONNECTED:
    default:                   dot_col = COL_ERR;   break;
    }
    /* 状态点 */
    fill_round_rect(8, 9, 10, 10, 3, dot_col);

    const char *mode_txt;
    switch (s_mode) {
    case UI_MODE_MOUSE:      mode_txt = "鼠标模式";   break;
    case UI_MODE_CALIBRATE:  mode_txt = "校准模式";   break;
    case UI_MODE_REMOTE:
    default:                 mode_txt = "遥控模式";   break;
    }
    draw_text(26, 6, mode_txt, COL_TEXT, 120);

    /* 右侧连接状态文字 */
    const char *conn_txt;
    switch (s_conn) {
    case UI_CONN_CONNECTED:    conn_txt = "已连接"; break;
    case UI_CONN_PAIRING:      conn_txt = "配对中"; break;
    default:                   conn_txt = "未连接"; break;
    }
    int tw = text_width(conn_txt);
    draw_text(SCR_W - 12 - tw, 6, conn_txt, dot_col, 100);
}

/** 主区：大号动作名 + 副标题 */
static void draw_main_area(void)
{
    /* 主区背景 */
    fill_rect(0, HEADER_H, SCR_W, FOOTER_Y - HEADER_H, COL_BG);

    /* 动作卡片 */
    int card_x = 16;
    int card_w = SCR_W - 32;
    int card_y = HEADER_H + 14;
    int card_h = 74;
    fill_round_rect(card_x, card_y, card_w, card_h, 8, COL_PANEL);
    /* 顶部强调条 */
    fill_rect(card_x, card_y, card_w, 3, COL_ACCENT);

    /* 主标题（居中） */
    int tw = text_width(s_title);
    int tx = (SCR_W - tw) / 2;
    if (tw > card_w - 16) {
        tx = card_x + 8;   /* 超宽则左对齐并截断 */
    }
    draw_text(tx, card_y + 18, s_title, COL_TEXT, card_w - 16);

    /* 副标题（居中，灰色） */
    if (s_subtitle[0]) {
        int sw = text_width(s_subtitle);
        int sx = (SCR_W - sw) / 2;
        if (sw > card_w - 16) {
            sx = card_x + 8;
        }
        draw_text(sx, card_y + 44, s_subtitle, COL_SUBTEXT, card_w - 16);
    }
}

/** 底部提示条 */
static void draw_footer(void)
{
    fill_rect(0, FOOTER_Y, SCR_W, SCR_H - FOOTER_Y, COL_PANEL);
    if (s_footer[0]) {
        draw_text(8, FOOTER_Y + 4, s_footer, COL_SUBTEXT, SCR_W - 16);
    }
}

static void redraw(void)
{
    if (!s_buf) {
        return;
    }
    fill_rect(0, 0, SCR_W, SCR_H, COL_BG);
    draw_header();
    draw_main_area();
    draw_footer();

    esp_lcd_panel_draw_bitmap(s_lcd.panel, 0, 0, SCR_W, SCR_H, s_buf);
    s_dirty = false;
}

/* ==================== 对外接口 ==================== */

esp_err_t ui_init(void)
{
    esp_err_t err = bsp_lcd_init(&s_lcd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 双缓冲放 PSRAM，320*240*2*2 = 300KB */
    s_buf = (uint16_t *)heap_caps_malloc(SCR_W * SCR_H * 2 * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) {
        /* 退化到内部 RAM */
        s_buf = (uint16_t *)malloc(SCR_W * SCR_H * 2 * sizeof(uint16_t));
        if (!s_buf) {
            ESP_LOGE(TAG, "framebuffer alloc failed");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "framebuffer in internal RAM");
    } else {
        ESP_LOGI(TAG, "framebuffer in PSRAM");
    }

    strcpy(s_title, "BiliMouse");
    strcpy(s_subtitle, "等待连接");
    strcpy(s_footer, "单击下一集  双击上一集  长按播放/暂停");

    bsp_lcd_set_backlight(80);
    s_dirty = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

void ui_set_conn_state(ui_conn_state_t state)
{
    if (s_conn != state) {
        s_conn = state;
        s_dirty = true;
    }
}

void ui_set_mode(ui_mode_t mode)
{
    if (s_mode != mode) {
        s_mode = mode;
        s_dirty = true;
    }
}

void ui_show_action(const char *title, const char *subtitle)
{
    if (title) {
        strncpy(s_title, title, sizeof(s_title) - 1);
        s_title[sizeof(s_title) - 1] = '\0';
    }
    if (subtitle) {
        strncpy(s_subtitle, subtitle, sizeof(s_subtitle) - 1);
        s_subtitle[sizeof(s_subtitle) - 1] = '\0';
    }
    s_dirty = true;
}

void ui_show_hint(const char *hint)
{
    if (hint) {
        strncpy(s_subtitle, hint, sizeof(s_subtitle) - 1);
        s_subtitle[sizeof(s_subtitle) - 1] = '\0';
    }
    s_dirty = true;
}

void ui_set_footer(const char *footer)
{
    if (footer) {
        strncpy(s_footer, footer, sizeof(s_footer) - 1);
        s_footer[sizeof(s_footer) - 1] = '\0';
    }
    s_dirty = true;
}

void ui_mark_dirty(void)
{
    s_dirty = true;
}

void ui_flush(void)
{
    if (s_dirty) {
        redraw();
    }
}
