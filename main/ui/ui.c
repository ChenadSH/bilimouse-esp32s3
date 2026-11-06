/**
 * @file ui.c
 * @brief BiliMouse 屏幕 UI 实现
 *
 * 自研轻量渲染器：RGB565 双缓冲 + 点阵字库，
 * 不引入 LVGL 等外部 GUI 框架，保证工程零外部依赖、离线可编译。
 *
 * 页面结构（v2.6）：
 *   首页    - emoji icon 左右滑动轮播，点 icon 进功能页
 *   B站页   - 只留播放控制（上下集 / 播放暂停 / 音量 / 全屏）
 *   电台页  - 二次元背景 + 半透明面板（台名/状态/控制按钮）
 *   其余页  - 红外 / 鼠标 / 校准 / 升级，各自独立
 *   跨页导航 - 统一走左上角「<」回首页，页面内不再堆其它功能的入口
 *
 * 布局（320x240）：
 *   y   0.. 26  顶栏：返回键 + 模式 + 连接状态
 *   y  30.. 88  动作卡片：最近一次动作（大字）
 *   y  96..160  主按钮行
 *   y 164..210  次按钮行
 *   y 218..240  底栏：提示
 */
#include "ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp_lcd.h"
#include "ui_font.h"
#include "ui_emoji.h"
#include "bg_radio.h"

#include "esp_app_desc.h"
#include "ota_update.h"
#include "radio.h"

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

/* 布局常量 */
#define HEADER_H        26
#define FOOTER_Y        218

/* 按钮区域定义 —— 触摸热区与绘制共用同一组坐标，保证所见即所点 */
typedef struct {
    ui_button_t id;
    int x, y, w, h;
    const char *label;
    uint16_t accent;
} btn_def_t;

/* ---- B站播放控制页（只保留播放相关功能） ----
 * 主按钮行 3 个大按钮，副按钮行 3 个。
 * 坐标写字面量：preview_ui.py 按数字解析按钮表，带参宏解析不了。 */
#define MAIN_BTN_Y      96
#define MAIN_BTN_H      64
#define SUB_BTN_Y       168
#define SUB_BTN_H       40
#define SUB_BTN_W       100

static const btn_def_t s_main_btns[] = {
    { UI_BTN_PREV_EPISODE,   6,  MAIN_BTN_Y, 100, MAIN_BTN_H, "上一集", COL_ACCENT },
    { UI_BTN_PLAY_PAUSE,   110,  MAIN_BTN_Y, 100, MAIN_BTN_H, "播放/暂停", COL_ACCENT },
    { UI_BTN_NEXT_EPISODE, 214,  MAIN_BTN_Y, 100, MAIN_BTN_H, "下一集", COL_ACCENT },
};

#define MAIN_BTN_CNT  (sizeof(s_main_btns) / sizeof(s_main_btns[0]))

static const btn_def_t s_sub_btns[] = {
    { UI_BTN_VOLUME_DOWN,     6, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "音量-", COL_SUBTEXT },
    { UI_BTN_VOLUME_UP,     112, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "音量+", COL_SUBTEXT },
    { UI_BTN_FULLSCREEN,    218, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "全屏",  COL_SUBTEXT },
};

#define SUB_BTN_CNT  (sizeof(s_sub_btns) / sizeof(s_sub_btns[0]))

/*
 * 鼠标模式：左右两侧各一个竖长按键，中间整块是触控板。
 *
 * 按键是「按住型」：手指按下立即发 mouse down，抬起才发 up，
 * 所以按住后往中间拖就是拖拽/框选，和真实鼠标一致。
 * 返回首页统一走左上角「<」，触控板不再被出口按钮占掉一块。
 */
#define MOUSE_BTN_W   58
#define MOUSE_BTN_Y   30
#define MOUSE_BTN_H   (FOOTER_Y - 8 - MOUSE_BTN_Y)   /* 30..210，footer 在 218 */

static const btn_def_t s_mouse_btns[] = {
    { UI_BTN_MOUSE_LEFT,   4,                        MOUSE_BTN_Y, MOUSE_BTN_W, MOUSE_BTN_H, "左键", COL_ACCENT },
    { UI_BTN_MOUSE_RIGHT,  SCR_W - 4 - MOUSE_BTN_W,  MOUSE_BTN_Y, MOUSE_BTN_W, MOUSE_BTN_H, "右键", COL_ACCENT },
};

#define MOUSE_BTN_CNT (sizeof(s_mouse_btns) / sizeof(s_mouse_btns[0]))

/* 红外模式：学习 / 发射 / 切换槽（返回走左上角） */
#define IR_BTN_Y   96
#define IR_BTN_H   56

static const btn_def_t s_ir_btns[] = {
    { UI_BTN_IR_LEARN,   6,   IR_BTN_Y, 150, IR_BTN_H, "学习", COL_ACCENT },
    { UI_BTN_IR_SEND,  164,   IR_BTN_Y, 150, IR_BTN_H, "发射", COL_OK },
    { UI_BTN_IR_SLOT,    6,   IR_BTN_Y + IR_BTN_H + 8, 150, 44, "换下一个槽", COL_SUBTEXT },
};

#define IR_BTN_CNT (sizeof(s_ir_btns) / sizeof(s_ir_btns[0]))

/* 电台模式：二次元背景 + 半透明面板（不画 footer，背景铺满全屏）。
   台名与状态用动作卡片的内容（radio.c 会往里写），但改成半透明。 */
#define RADIO_BTN_Y  104

static const btn_def_t s_radio_btns[] = {
    { UI_BTN_RADIO_PREV,    10,  RADIO_BTN_Y,      94, 46, "上一台",   COL_ACCENT },
    { UI_BTN_RADIO_TOGGLE, 112,  RADIO_BTN_Y,      94, 46, "播放/停止", COL_ACCENT },
    { UI_BTN_RADIO_NEXT,   214,  RADIO_BTN_Y,      94, 46, "下一台",   COL_ACCENT },
    { UI_BTN_RADIO_LIST,    10,  RADIO_BTN_Y + 54, 298, 40, "台列表",  COL_SUBTEXT },
};

#define RADIO_BTN_CNT (sizeof(s_radio_btns) / sizeof(s_radio_btns[0]))

/* 台列表：每行 21px，点行选台。行区域是动态命中的（见 ui_hit_test）。
   返回统一走左上角「< 返回」，这里不再放「返回电台」按钮 ——
   一屏出现两个返回入口只会让人猜。 */
#define RLIST_ROW_Y    32
#define RLIST_ROW_H    21
#define RLIST_MAX      9

/* 升级页：两个按钮并排。「立即升级」只在检查到新版本后绘制/可点，
   严格执行「检测到新版本才能更新」。 */
static const btn_def_t s_ota_btns[] = {
    { UI_BTN_OTA_START,      6, 168, 148, 40, "开始检查更新", COL_ACCENT },
};

static const btn_def_t s_ota_btns_ready[] = {
    { UI_BTN_OTA_START,      6, 168, 148, 40, "开始检查更新", COL_ACCENT },
    { UI_BTN_OTA_DOWNLOAD, 164, 168, 150, 40, "立即升级",     COL_OK },
};

#define OTA_BTN_CNT       (sizeof(s_ota_btns) / sizeof(s_ota_btns[0]))
#define OTA_BTN_READY_CNT (sizeof(s_ota_btns_ready) / sizeof(s_ota_btns_ready[0]))

/* 校准模式按钮：两个大按钮 + 一个完成。
   完成按钮必须完整落在底栏(218)之上。 */
#define CAL_BTN_Y      96
#define CAL_BTN_H      64

static const btn_def_t s_cal_btns[] = {
    { UI_BTN_CAL_NEXT,   6,  CAL_BTN_Y, 150, CAL_BTN_H, "校准下一集", COL_ACCENT },
    { UI_BTN_CAL_PREV, 164,  CAL_BTN_Y, 150, CAL_BTN_H, "校准上一集", COL_ACCENT },
    { UI_BTN_CAL_EXIT, 80,  CAL_BTN_Y + CAL_BTN_H + 8, 160, 40, "完成", COL_OK },
};

#define CAL_BTN_CNT  (sizeof(s_cal_btns) / sizeof(s_cal_btns[0]))

static bsp_lcd_t s_lcd;
static uint16_t *s_buf = NULL;     /* 双缓冲 */
static bool s_dirty = true;

static ui_conn_state_t s_conn = UI_CONN_DISCONNECTED;
static ui_mode_t s_mode = UI_MODE_HOME;
static char s_title[32] = "BiliMouse";
static char s_subtitle[48] = "";
static char s_footer[48] = "";

static ui_button_cb_t s_btn_cb;
static void *s_btn_ctx;

/* 前向声明（draw_home 等在 draw_frame 定义前使用） */
static void draw_frame(int x, int y, int w, int h, int t, uint16_t color);

/* 触摸校准界面状态（UI_MODE_TOUCH_CAL 时使用） */
static ui_touch_cal_state_t s_tcal = { .stage = -1 };

/* ---- 首页 emoji 轮播 ----
 * 一屏一个 icon，左右滑动切换；点 icon 进对应功能页。
 * 页序 = home_icon_t 枚举顺序 = ui_emoji_t 顺序。 */
static int s_home_page;

static const struct {
    home_icon_t icon;
    const char *name;     /* icon 下方标题 */
} s_home_icons[HOME_ICON_COUNT] = {
    { HOME_ICON_BILI,  "B站视频控制" },
    { HOME_ICON_MOUSE, "鼠标模拟" },
    { HOME_ICON_IR,    "红外模拟" },
    { HOME_ICON_RADIO, "网络电台" },
    { HOME_ICON_OTA,   "在线升级" },
    { HOME_ICON_CAL,   "触摸校准" },
};

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

/** 半透明矩形：与帧缓冲里已有的颜色按 alpha (0..255) 混合。
 *  电台页的背景图上叠面板/按钮全靠它，否则一整块实色面板
 *  会把精心生成的二次元背景糊死。 */
static void blend_rect(int x, int y, int w, int h, uint16_t color, int alpha)
{
    if (alpha >= 255) {
        fill_rect(x, y, w, h, color);
        return;
    }
    if (alpha <= 0) {
        return;
    }
    int cr = (color >> 11) & 0x1F;
    int cg = (color >> 5) & 0x3F;
    int cb = color & 0x1F;
    for (int j = y; j < y + h; j++) {
        if (j < 0 || j >= SCR_H) {
            continue;
        }
        for (int i = x; i < x + w; i++) {
            if (i < 0 || i >= SCR_W) {
                continue;
            }
            uint16_t c = s_buf[j * SCR_W + i];
            int r = (c >> 11) & 0x1F;
            int g = (c >> 5) & 0x3F;
            int b = c & 0x1F;
            r = (r * (255 - alpha) + cr * alpha) / 255;
            g = (g * (255 - alpha) + cg * alpha) / 255;
            b = (b * (255 - alpha) + cb * alpha) / 255;
            s_buf[j * SCR_W + i] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    fill_rect(x + r, y, w - 2 * r, h, color);
    fill_rect(x, y + r, r, h - 2 * r, color);
    fill_rect(x + w - r, y + r, r, h - 2 * r, color);
    fill_rect(x + r, y + r, w - 2 * r, r, color);
    fill_rect(x + r, y + h - r, w - 2 * r, r, color);
}

/** 半透明圆角矩形（四角保留背景，混合同 fill_round_rect 的分块方式） */
static void blend_round_rect(int x, int y, int w, int h, int r, uint16_t color, int alpha)
{
    blend_rect(x + r, y, w - 2 * r, h, color, alpha);
    blend_rect(x, y + r, r, h - 2 * r, color, alpha);
    blend_rect(x + w - r, y + r, r, h - 2 * r, color, alpha);
    blend_rect(x + r, y + r, w - 2 * r, r, color, alpha);
    blend_rect(x + r, y + h - r, w - 2 * r, r, color, alpha);
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

/* 左上角返回键尺寸：整条顶栏高度，宽 58px。
   之前只画了一个 8 像素宽的 "<" 字符，热区靠「点顶栏任意位置」兜着，
   但视觉上完全看不出那是按钮 —— 用户就去找右上角、左滑之类的野路子。
   现在画成实心胶囊 + 「< 返回」文字，一眼可辨、躺着单手也能戳中。 */
#define BACK_W   58

/** 顶部状态栏：返回键 + 连接状态点 + 模式文字。
    非首页模式左上角是唯一的「回首页」入口，其余任何位置的返回入口都已移除。 */
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

    const char *mode_txt;
    switch (s_mode) {
    case UI_MODE_HOME:       mode_txt = "首页";       break;
    case UI_MODE_REMOTE:     mode_txt = "B站控制";    break;
    case UI_MODE_MOUSE:      mode_txt = "鼠标模式";   break;
    case UI_MODE_CALIBRATE:  mode_txt = "校准模式";   break;
    case UI_MODE_TOUCH_CAL:  mode_txt = "触摸校准";   break;
    case UI_MODE_IR:         mode_txt = "红外遥控";   break;
    case UI_MODE_RADIO:      mode_txt = "网络电台";   break;
    case UI_MODE_RADIO_LIST: mode_txt = "选择电台";   break;
    case UI_MODE_OTA_PAGE:   mode_txt = "在线升级";   break;
    default:                 mode_txt = "";           break;
    }

    const char *conn_txt;
    switch (s_conn) {
    case UI_CONN_CONNECTED:    conn_txt = "已连接"; break;
    case UI_CONN_PAIRING:      conn_txt = "配对中"; break;
    default:                   conn_txt = "未连接"; break;
    }
    int conn_tw = text_width(conn_txt);
    draw_text(SCR_W - 8 - conn_tw, 5, conn_txt, dot_col, 100);

    if (s_mode == UI_MODE_HOME) {
        fill_round_rect(8, 8, 10, 10, 3, dot_col);
        draw_text(24, 5, mode_txt, COL_TEXT, 150);
        return;
    }

    /* 左上角「< 返回」胶囊：点它回首页（命中区见 ui_hit_test） */
    fill_round_rect(2, 2, BACK_W, HEADER_H - 4, 6, COL_ACCENT);
    draw_text(8, 5, "<", COL_TEXT, 16);
    draw_text(20, 5, "返回", COL_TEXT, 36);

    fill_round_rect(66, 8, 10, 10, 3, dot_col);
    draw_text(82, 5, mode_txt, COL_TEXT, 120);
}

/** 动作卡片：最近一次动作（可指定横向范围与透明度） */
static void draw_action_card_in(int card_x, int card_w, int alpha)
{
    int card_y = HEADER_H + 4;
    int card_h = 58;

    if (alpha >= 255) {
        fill_round_rect(card_x, card_y, card_w, card_h, 8, COL_PANEL);
    } else {
        blend_round_rect(card_x, card_y, card_w, card_h, 8, COL_PANEL, alpha);
    }
    fill_rect(card_x, card_y, card_w, 3, COL_ACCENT);

    /* 主标题（居中） */
    int tw = text_width(s_title);
    int tx = card_x + (card_w - tw) / 2;
    if (tw > card_w - 16) {
        tx = card_x + 8;
    }
    draw_text(tx, card_y + 10, s_title, COL_TEXT, card_w - 16);

    /* 副标题（居中，灰色） */
    if (s_subtitle[0]) {
        int sw = text_width(s_subtitle);
        int sx = card_x + (card_w - sw) / 2;
        if (sw > card_w - 16) {
            sx = card_x + 8;
        }
        draw_text(sx, card_y + 32, s_subtitle, COL_SUBTEXT, card_w - 16);
    }
}

static void draw_action_card(void)
{
    draw_action_card_in(6, SCR_W - 12, 255);
}

/** 当前页按钮是否画成半透明（电台页的按钮叠在背景图上） */
static int s_btn_alpha = 255;

/** 带文字的按钮 */
static void draw_button(int x, int y, int w, int h, const char *label, uint16_t accent)
{
    if (s_btn_alpha >= 255) {
        fill_round_rect(x, y, w, h, 8, COL_PANEL);
    } else {
        blend_round_rect(x, y, w, h, 8, COL_PANEL, s_btn_alpha);
    }
    /* 上下强调条，让按钮边界清晰可辨 */
    fill_rect(x + 8, y, w - 16, 3, accent);
    fill_rect(x + 8, y + h - 3, w - 16, 3, accent);

    int tw = text_width(label);
    int tx = x + (w - tw) / 2;
    if (tx < x + 2) {
        tx = x + 2;
    }
    int ty = y + (h - 16) / 2;
    draw_text(tx, ty, label, COL_TEXT, w - 4);
}

static void draw_btn_group(const btn_def_t *btns, size_t cnt)
{
    for (size_t i = 0; i < cnt; i++) {
        draw_button(btns[i].x, btns[i].y, btns[i].w, btns[i].h,
                    btns[i].label, btns[i].accent);
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

/* ==================== 首页（emoji 轮播） ==================== */

/* 磁贴水平居中、垂直方向也居中：屏幕 320x240，96px 磁贴 ->
   x=112, y=72，磁贴中心正好落在屏幕中心 (160,120)。
   底下只留功能名与页点，不画底栏 —— 首页是纯图标页。 */
#define HOME_TILE_SZ  96
#define HOME_TILE_X   ((SCR_W - HOME_TILE_SZ) / 2)
#define HOME_TILE_Y   ((SCR_H - HOME_TILE_SZ) / 2)
#define HOME_NAME_Y   (HOME_TILE_Y + HOME_TILE_SZ + 10)
#define HOME_DOT_Y    204
#define HOME_EMOJI_O  ((HOME_TILE_SZ - UI_EMOJI_SZ) / 2)   /* 12 */

static const uint16_t *home_emoji_bitmap(int idx)
{
    switch (s_home_icons[idx].icon) {
    case HOME_ICON_BILI:  return ui_emoji_bili;
    case HOME_ICON_MOUSE: return ui_emoji_mouse;
    case HOME_ICON_IR:    return ui_emoji_ir;
    case HOME_ICON_RADIO: return ui_emoji_radio;
    case HOME_ICON_OTA:   return ui_emoji_ota;
    case HOME_ICON_CAL:   return ui_emoji_cal;
    default:              return ui_emoji_bili;
    }
}

/** 整块贴 72x72 的预合成 emoji（emoji 自带磁贴底色，与底色无缝衔接） */
static void blit_emoji(int x, int y, const uint16_t *bmp)
{
    for (int j = 0; j < UI_EMOJI_SZ; j++) {
        int dy = y + j;
        if (dy < 0 || dy >= SCR_H) {
            continue;
        }
        memcpy(s_buf + dy * SCR_W + x, bmp + j * UI_EMOJI_SZ,
               UI_EMOJI_SZ * sizeof(uint16_t));
    }
}

static void draw_home(void)
{
    const uint16_t *emoji = home_emoji_bitmap(s_home_page);
    uint16_t tile_bg = ui_emoji_tile_bg[s_home_page];
    uint16_t border = ui_emoji_tile_border[s_home_page];

    /* 大磁贴 + 描边 + emoji */
    fill_round_rect(HOME_TILE_X, HOME_TILE_Y, HOME_TILE_SZ, HOME_TILE_SZ, 14, tile_bg);
    blit_emoji(HOME_TILE_X + HOME_EMOJI_O, HOME_TILE_Y + HOME_EMOJI_O, emoji);
    draw_frame(HOME_TILE_X, HOME_TILE_Y, HOME_TILE_SZ, HOME_TILE_SZ, 2, border);

    /* 功能名（磁贴正下方） */
    const char *name = s_home_icons[s_home_page].name;
    int nw = text_width(name);
    draw_text((SCR_W - nw) / 2, HOME_NAME_Y, name, COL_TEXT, 200);

    /* 页点 */
    for (int i = 0; i < HOME_ICON_COUNT; i++) {
        int dx = (SCR_W - HOME_ICON_COUNT * 14) / 2 + i * 14 + 4;
        fill_round_rect(dx, HOME_DOT_Y, 6, 6, 3,
                        i == s_home_page ? COL_ACCENT : COL_SUBTEXT);
    }

    /* 左右滑动提示：贴在磁贴两侧（与磁贴同一水平中线），不在底部堆文字 */
    draw_text(10, HOME_TILE_Y + HOME_TILE_SZ / 2 - 8, "<", COL_SUBTEXT, 20);
    draw_text(SCR_W - 22, HOME_TILE_Y + HOME_TILE_SZ / 2 - 8, ">", COL_SUBTEXT, 20);
}

/* ==================== 电台页（背景图版） ==================== */

static void draw_radio_bg(void)
{
    /* 逐行拷贝背景（flash 常量 -> 帧缓冲），比逐像素快 */
    for (int y = 0; y < SCR_H; y++) {
        memcpy(s_buf + y * SCR_W, bg_radio + y * SCR_W, SCR_W * sizeof(uint16_t));
    }
}

static void draw_radio_list(void)
{
    /* 列表占满内容区，不画动作卡片（否则和行文字重叠） */
    int cnt = radio_station_count();
    if (cnt <= 0) {
        draw_text(48, 110, "台列表为空，点左上角返回重进电台", COL_SUBTEXT, 240);
        return;
    }
    if (cnt > RLIST_MAX) {
        cnt = RLIST_MAX;
    }
    int cur = radio_current_index();
    for (int i = 0; i < cnt; i++) {
        const radio_station_t *st = radio_station(i);
        if (!st) {
            break;
        }
        int ry = RLIST_ROW_Y + i * RLIST_ROW_H;
        bool sel = (i == cur);
        if (sel) {
            fill_round_rect(6, ry - 2, 308, RLIST_ROW_H, 5, COL_PANEL);
            fill_rect(6, ry - 2, 3, RLIST_ROW_H, COL_ACCENT);
        }
        char line[56];
        snprintf(line, sizeof(line), "%d. %s", i + 1, st->name);
        draw_text(16, ry + 1, line, sel ? COL_ACCENT : COL_TEXT, 250);
        if (sel && radio_is_playing()) {
            draw_text(286, ry + 1, ">", COL_OK, 20);
        }
    }
}

/* ==================== 升级页 ==================== */

/** 画一行「步骤名 + 结果」，颜色按成功/失败/进行中区分 */
static void ota_step_row(int y, const char *name, const char *result, int color)
{
    draw_text(16, y, name, COL_TEXT, 70);
    draw_text(86, y, result, color, 220);
}

static void draw_ota_page(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ota_state_t st;
    ota_update_get_state(&st);

    fill_round_rect(6, 32, 308, 126, 8, COL_PANEL);

    char line[96];
    snprintf(line, sizeof(line), "当前版本: %.40s", app->version);
    draw_text(16, 42, line, COL_TEXT, 290);

    /* 步骤 1：WiFi —— 失败红、成功绿、没查过灰 */
    int wifi_col = COL_SUBTEXT;
    if (st.phase == OTA_PHASE_FAIL && strstr(st.wifi_info, "失败")) {
        wifi_col = COL_ERR;
    } else if (st.phase != OTA_PHASE_IDLE && st.phase != OTA_PHASE_WIFI) {
        wifi_col = COL_OK;
    } else if (st.phase == OTA_PHASE_WIFI) {
        wifi_col = COL_WARN;
    }
    ota_step_row(64, "WiFi:", st.wifi_info[0] ? st.wifi_info : "未检查", wifi_col);

    /* 步骤 2：版本清单 */
    int list_col = COL_SUBTEXT;
    if (strcmp(st.list_info, "获取失败") == 0) {
        list_col = COL_ERR;
    } else if (strcmp(st.list_info, "已获取") == 0) {
        list_col = COL_OK;
    } else if (st.phase == OTA_PHASE_MANIFEST) {
        list_col = COL_WARN;
    }
    ota_step_row(84, "清单:", st.list_info[0] ? st.list_info : "未检查", list_col);

    /* 步骤 3：版本比较结果 */
    int cmp_col = COL_SUBTEXT;
    if (st.phase == OTA_PHASE_READY) {
        cmp_col = COL_WARN;
    } else if (st.phase == OTA_PHASE_LATEST) {
        cmp_col = COL_OK;
    } else if (st.phase == OTA_PHASE_FAIL) {
        cmp_col = COL_ERR;
    }
    const char *cmp_txt = st.cmp_info[0] ? st.cmp_info : "未检查";
    ota_step_row(104, "版本:", cmp_txt, cmp_col);

    /* 下载进度条（仅下载阶段显示） */
    if (st.phase == OTA_PHASE_DOWNLOAD && st.percent >= 0) {
        draw_frame(16, 128, 288, 16, 2, COL_ACCENT);
        fill_rect(18, 130, (int)(284 * (st.percent < 100 ? st.percent : 100)) / 100, 12, COL_ACCENT);
        snprintf(line, sizeof(line), "下载 %d%%  %d/%d KB",
                 st.percent, st.done_kb, st.total_kb);
        draw_text(16, 148, line, COL_TEXT, 290);
    } else if (st.phase == OTA_PHASE_READY && st.total_kb > 0) {
        snprintf(line, sizeof(line), "升级包: %d KB", st.total_kb);
        draw_text(16, 130, line, COL_SUBTEXT, 290);
    } else if (st.phase == OTA_PHASE_FAIL) {
        draw_text(16, 130, "失败不影响当前使用，可重试", COL_SUBTEXT, 290);
    }

    draw_btn_group(st.phase == OTA_PHASE_READY ? s_ota_btns_ready : s_ota_btns,
                   st.phase == OTA_PHASE_READY ? OTA_BTN_READY_CNT : OTA_BTN_CNT);
}

/* ==================== 触摸校准界面 ==================== */

/** 空心矩形边框 */
static void draw_frame(int x, int y, int w, int h, int t, uint16_t color)
{
    fill_rect(x, y, w, t, color);
    fill_rect(x, y + h - t, w, t, color);
    fill_rect(x, y, t, h, color);
    fill_rect(x + w - t, y, t, h, color);
}

/** 十字准星 */
static void draw_cross(int cx, int cy, int arm, int t, uint16_t color)
{
    fill_rect(cx - arm, cy - t / 2, arm * 2 + t, t, color);
    fill_rect(cx - t / 2, cy - arm, t, arm * 2 + t, color);
}

static void draw_touch_cal(void)
{
    if (s_tcal.stage < 0) {
        return;
    }

    if (s_tcal.stage < 3) {
        /* 目标框：44x44 外框 + 正中实心块，手指粗也能盖住 */
        int bx = s_tcal.target_x - 22;
        int by = s_tcal.target_y - 22;
        draw_frame(bx, by, 44, 44, 3, COL_ERR);
        fill_rect(s_tcal.target_x - 5, s_tcal.target_y - 5, 10, 10, COL_ERR);
    }

    if (s_tcal.has_point) {
        draw_cross(s_tcal.point_x, s_tcal.point_y, 14, 3, COL_OK);
    }
}

static void redraw(void)
{
    if (!s_buf) {
        return;
    }
    if (s_mode == UI_MODE_RADIO) {
        draw_radio_bg();       /* 整屏背景图代替纯色底 */
        s_btn_alpha = 170;     /* 面板/按钮半透明，让背景透出来 */
    } else {
        fill_rect(0, 0, SCR_W, SCR_H, COL_BG);
        s_btn_alpha = 255;
    }
    draw_header();

    switch (s_mode) {
    case UI_MODE_HOME:
        draw_home();
        break;
    case UI_MODE_TOUCH_CAL:
        draw_action_card();
        draw_touch_cal();
        break;
    case UI_MODE_CALIBRATE:
        draw_action_card();
        draw_btn_group(s_cal_btns, CAL_BTN_CNT);
        break;
    case UI_MODE_REMOTE:
        draw_action_card();
        draw_btn_group(s_main_btns, MAIN_BTN_CNT);
        draw_btn_group(s_sub_btns, SUB_BTN_CNT);
        break;
    case UI_MODE_IR:
        draw_action_card();
        draw_btn_group(s_ir_btns, IR_BTN_CNT);
        break;
    case UI_MODE_RADIO:
        /* 电台页：半透明动作卡片显示台名/状态（radio.c 会写进来） */
        draw_action_card_in(10, 300, s_btn_alpha);
        draw_btn_group(s_radio_btns, RADIO_BTN_CNT);
        break;
    case UI_MODE_RADIO_LIST:
        draw_radio_list();
        break;
    case UI_MODE_OTA_PAGE:
        draw_ota_page();
        break;
    default:
        /* 鼠标模式：左右两个按住型按键，中间是触控板。
           卡片缩窄到中间，避免被按键压住。 */
        draw_action_card_in(70, SCR_W - 140, 255);
        draw_btn_group(s_mouse_btns, MOUSE_BTN_CNT);
        break;
    }

    /* 首页是纯图标页：不画底栏（底部留白，图标才能在屏幕正中）；
       电台/台列表用全屏背景图，同样不画底栏。 */
    if (s_mode != UI_MODE_RADIO && s_mode != UI_MODE_RADIO_LIST &&
        s_mode != UI_MODE_HOME) {
        draw_footer();
    }

    /* ⚠️ 必须分条刷新，不能一次 draw_bitmap 整帧：
       esp_lcd 把 PSRAM 帧缓冲交给 SPI DMA 时，spicommon 会为每次传输
       临时 malloc 一块**内部**对齐缓冲（PSRAM 指针不满足 cache 对齐就
       得先拷一份）。整帧 320*240*2=150KB，运行期内部 RAM 只剩几 KB，
       分配必然失败 —— 串口刷屏 "spicommon_dma_setup_priv_buffer
       Failed to allocate priv TX buffer"，屏幕上画面停住不动。
       每条 4 行 = 320*4*2 = 2560 字节，能塞进当前最大的 4KB 内部块；
       整帧 60 条，SPI 80MHz 下总耗时只有几毫秒量级。 */
    #define FLUSH_ROWS 4
    for (int y = 0; y < SCR_H; y += FLUSH_ROWS) {
        int h = (y + FLUSH_ROWS <= SCR_H) ? FLUSH_ROWS : (SCR_H - y);
        esp_lcd_panel_draw_bitmap(s_lcd.panel, 0, y, SCR_W, y + h,
                                  s_buf + (size_t)y * SCR_W);
    }
    s_dirty = false;
}

/* ==================== 命中测试 ==================== */

static ui_button_t hit_group(const btn_def_t *btns, size_t cnt, uint16_t x, uint16_t y)
{
    for (size_t i = 0; i < cnt; i++) {
        if ((int)x >= btns[i].x && (int)x < btns[i].x + btns[i].w &&
            (int)y >= btns[i].y && (int)y < btns[i].y + btns[i].h) {
            return btns[i].id;
        }
    }
    return UI_BTN_NONE;
}

ui_button_t ui_hit_test(uint16_t x, uint16_t y)
{
    if (s_mode == UI_MODE_TOUCH_CAL) {
        /* 校准向导独占触摸：任何点击都算向导输入 */
        return UI_BTN_NONE;
    }
    if (s_mode == UI_MODE_HOME) {
        /* 大磁贴热区（略放大，好点），含下方功能名 */
        if (x >= HOME_TILE_X - 16 && x < HOME_TILE_X + HOME_TILE_SZ + 16 &&
            y >= HOME_TILE_Y - 6 && y < HOME_TILE_Y + HOME_TILE_SZ + 30) {
            return UI_BTN_HOME_ICON0;
        }
        if (x < 70) {
            return UI_BTN_HOME_PREV;
        }
        if (x >= SCR_W - 70) {
            return UI_BTN_HOME_NEXT;
        }
        return UI_BTN_NONE;
    }
    /* 非首页：点顶部状态栏 = 返回首页（全设备统一的返回入口） */
    if (y < HEADER_H) {
        return UI_BTN_HOME;
    }
    if (s_mode == UI_MODE_CALIBRATE) {
        return hit_group(s_cal_btns, CAL_BTN_CNT, x, y);
    }
    if (s_mode == UI_MODE_REMOTE) {
        ui_button_t b = hit_group(s_main_btns, MAIN_BTN_CNT, x, y);
        if (b != UI_BTN_NONE) {
            return b;
        }
        return hit_group(s_sub_btns, SUB_BTN_CNT, x, y);
    }
    if (s_mode == UI_MODE_IR) {
        return hit_group(s_ir_btns, IR_BTN_CNT, x, y);
    }
    if (s_mode == UI_MODE_RADIO) {
        return hit_group(s_radio_btns, RADIO_BTN_CNT, x, y);
    }
    if (s_mode == UI_MODE_RADIO_LIST) {
        int cnt = radio_station_count();
        if (cnt > RLIST_MAX) {
            cnt = RLIST_MAX;
        }
        if (x >= 6 && x < 314 && y >= RLIST_ROW_Y - 2 &&
            y < RLIST_ROW_Y + cnt * RLIST_ROW_H) {
            int idx = (y - RLIST_ROW_Y + 2) / RLIST_ROW_H;
            if (idx >= 0 && idx < cnt) {
                return (ui_button_t)(UI_BTN_RADIO_PICK0 + idx);
            }
        }
        return UI_BTN_NONE;
    }
    if (s_mode == UI_MODE_OTA_PAGE) {
        /* 「立即升级」只在检测到新版本后出现，命中区跟着收放 */
        ota_state_t st;
        ota_update_get_state(&st);
        if (st.phase == OTA_PHASE_READY) {
            return hit_group(s_ota_btns_ready, OTA_BTN_READY_CNT, x, y);
        }
        return hit_group(s_ota_btns, OTA_BTN_CNT, x, y);
    }
    /* 鼠标模式：左右按键优先，其余区域都是触控板 */
    return hit_group(s_mouse_btns, MOUSE_BTN_CNT, x, y);
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
    strcpy(s_footer, "左右滑动选功能，点图标打开");

    bsp_lcd_set_backlight(80);
    s_dirty = true;
    ESP_LOGI(TAG, "initialized");
    return ESP_OK;
}

void ui_set_button_callback(ui_button_cb_t cb, void *ctx)
{
    s_btn_cb = cb;
    s_btn_ctx = ctx;
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

bool ui_mode_is_home(void)
{
    return s_mode == UI_MODE_HOME;
}

bool ui_mode_uses_pointer_click(void)
{
    return s_mode == UI_MODE_REMOTE || s_mode == UI_MODE_MOUSE;
}

/* ==================== 首页轮播 ==================== */

int ui_home_page(void)
{
    return s_home_page;
}

int ui_home_page_count(void)
{
    return HOME_ICON_COUNT;
}

static void home_page_move(int delta)
{
    int p = s_home_page + delta;
    if (p < 0) {
        p = HOME_ICON_COUNT - 1;
    } else if (p >= HOME_ICON_COUNT) {
        p = 0;
    }
    if (p != s_home_page) {
        s_home_page = p;
        s_dirty = true;
    }
}

void ui_home_prev_page(void)
{
    home_page_move(-1);
}

void ui_home_next_page(void)
{
    home_page_move(1);
}

home_icon_t ui_home_current_icon(void)
{
    return s_home_icons[s_home_page].icon;
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

void ui_touch_cal_render(const ui_touch_cal_state_t *st)
{
    if (st == NULL) {
        return;
    }

    bool changed = (st->stage != s_tcal.stage) ||
                   (st->has_point != s_tcal.has_point);
    /* 十字每帧都在动，位移超过 2 像素才重绘，避免无谓刷屏 */
    if (!changed && st->has_point) {
        int dx = st->point_x - s_tcal.point_x;
        int dy = st->point_y - s_tcal.point_y;
        changed = (dx > 2 || dx < -2 || dy > 2 || dy < -2);
    }
    if (!changed && st->stage < 3 && (st->target_x != s_tcal.target_x ||
                                      st->target_y != s_tcal.target_y)) {
        changed = true;
    }
    if (!changed) {
        return;
    }

    s_tcal = *st;
    s_dirty = true;
}
