/**
 * @file ui.c
 * @brief BiliMouse 屏幕 UI 实现
 *
 * 自研轻量渲染器：RGB565 双缓冲 + 点阵字库，
 * 不引入 LVGL 等外部 GUI 框架，保证工程零外部依赖、离线可编译。
 *
 * 布局（320x240）：
 *   y   0.. 26  顶栏：连接状态 + 模式
 *   y  30.. 92  动作卡片：最近一次动作（大字）
 *   y  96..160  主按钮行：上一集 / 下一集 / 播放暂停（大热区）
 *   y 164..214  次按钮行：音量- / 音量+ / 全屏 / 鼠标
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
#include "bg_radio.h"

#include "esp_app_desc.h"
#include "ota_update.h"
#include "nav_alert.h"
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

/* ---- B站控制页（独立功能页） ----
 * 主按钮行（三个大按钮），副按钮行 9 个小按钮：
 * 音- 音+ 全屏 鼠标 电台 红外 导航 校准 升级
 * 坐标写字面量：preview_ui.py 按数字解析按钮表，带参宏解析不了。 */
#define MAIN_BTN_Y      96
#define MAIN_BTN_H      64
#define SUB_BTN_Y       160
#define SUB_BTN_H       54
#define SUB_BTN_W       34

static const btn_def_t s_main_btns[] = {
    { UI_BTN_PREV_EPISODE,   6,  MAIN_BTN_Y, 100, MAIN_BTN_H, "上一集", COL_ACCENT },
    { UI_BTN_NEXT_EPISODE, 110,  MAIN_BTN_Y, 100, MAIN_BTN_H, "下一集", COL_ACCENT },
    { UI_BTN_PLAY_PAUSE,   214,  MAIN_BTN_Y, 100, MAIN_BTN_H, "播放/暂停", COL_ACCENT },
};

#define MAIN_BTN_CNT  (sizeof(s_main_btns) / sizeof(s_main_btns[0]))

static const btn_def_t s_sub_btns[] = {
    { UI_BTN_VOLUME_DOWN,     2, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "音-",   COL_SUBTEXT },
    { UI_BTN_VOLUME_UP,      37, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "音+",   COL_SUBTEXT },
    { UI_BTN_FULLSCREEN,     72, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "全屏",  COL_SUBTEXT },
    { UI_BTN_MOUSE_MODE,    107, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "鼠标",  COL_SUBTEXT },
    { UI_BTN_RADIO_MODE,    142, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "电台",  COL_SUBTEXT },
    { UI_BTN_IR_MODE,       177, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "红外",  COL_SUBTEXT },
    { UI_BTN_NAV_MODE,      212, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "导航",  COL_SUBTEXT },
    { UI_BTN_TOUCH_CAL,     247, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "校准",  COL_SUBTEXT },
    { UI_BTN_OTA,           282, SUB_BTN_Y, SUB_BTN_W, SUB_BTN_H, "升级",  COL_SUBTEXT },
};

#define SUB_BTN_CNT  (sizeof(s_sub_btns) / sizeof(s_sub_btns[0]))

/*
 * 鼠标模式：左右两侧各一个竖长按键，中间整块是触控板。
 *
 * 按键是「按住型」：手指按下立即发 mouse down，抬起才发 up，
 * 所以按住后往中间拖就是拖拽/框选，和真实鼠标一致。
 * 两侧留 58px，手指粗也能按准；中间 196px 宽足够划动指针。
 * 高度只到内容区为止（FOOTER_Y 上方留 8px），不压到底部。
 */
#define MOUSE_BTN_W   58
#define MOUSE_BTN_Y   30
#define MOUSE_BTN_H   (FOOTER_Y - 8 - MOUSE_BTN_Y)   /* 30..210，footer 在 218 */

static const btn_def_t s_mouse_btns[] = {
    { UI_BTN_MOUSE_LEFT,   4,                    MOUSE_BTN_Y, MOUSE_BTN_W, MOUSE_BTN_H, "左键", COL_ACCENT },
    { UI_BTN_MOUSE_RIGHT,  SCR_W - 4 - MOUSE_BTN_W, MOUSE_BTN_Y, MOUSE_BTN_W, MOUSE_BTN_H, "右键", COL_ACCENT },
    /* 返回首页的出口：必须留着，否则进了鼠标模式就点不到别的按钮 */
    { UI_BTN_HOME,         132,                  160,          56,           44,          "首页", COL_SUBTEXT },
};

#define MOUSE_BTN_CNT (sizeof(s_mouse_btns) / sizeof(s_mouse_btns[0]))

/* 红外模式：学习 / 发射 / 切换槽 / 返回 */
#define IR_BTN_Y   112
#define IR_BTN_H   56

static const btn_def_t s_ir_btns[] = {
    { UI_BTN_IR_LEARN,   6,   IR_BTN_Y, 150, IR_BTN_H, "学习",   COL_ACCENT },
    { UI_BTN_IR_SEND,  164,   IR_BTN_Y, 150, IR_BTN_H, "发射",   COL_OK },
    { UI_BTN_IR_SLOT,    6,   IR_BTN_Y + IR_BTN_H + 8, 150, 44, "换下一个槽", COL_SUBTEXT },
    { UI_BTN_IR_BACK,  164,   IR_BTN_Y + IR_BTN_H + 8, 150, 44, "首页",   COL_SUBTEXT },
};

#define IR_BTN_CNT (sizeof(s_ir_btns) / sizeof(s_ir_btns[0]))

/* 电台模式：二次元背景 + 底部控制条（不画 footer，按钮占满底部）。
   台名与状态显示在上半区卡片；台列表是独立页（UI_MODE_RADIO_LIST）。 */
#define RADIO_BTN_Y  158

static const btn_def_t s_radio_btns[] = {
    { UI_BTN_RADIO_PREV,     6,  RADIO_BTN_Y,      100, 32, "上一台",    COL_SUBTEXT },
    { UI_BTN_RADIO_NEXT,   214,  RADIO_BTN_Y,      100, 32, "下一台",    COL_SUBTEXT },
    { UI_BTN_RADIO_TOGGLE,   6,  RADIO_BTN_Y + 38, 100, 32, "播放/停止", COL_ACCENT },
    { UI_BTN_RADIO_LIST,   112,  RADIO_BTN_Y + 38,  96, 32, "台列表",    COL_SUBTEXT },
    { UI_BTN_HOME,         214,  RADIO_BTN_Y + 38, 100, 32, "首页",      COL_SUBTEXT },
};

#define RADIO_BTN_CNT (sizeof(s_radio_btns) / sizeof(s_radio_btns[0]))

/* 台列表：每行 21px，点行选台。行区域是动态命中的（见 ui_hit_test）。 */
#define RLIST_ROW_Y    32
#define RLIST_ROW_H    21
#define RLIST_MAX      8

static const btn_def_t s_rlist_btns[] = {
    { UI_BTN_RADIO_LIST_BACK, 6, 210, 308, 26, "返回", COL_SUBTEXT },
};

#define RLIST_BTN_CNT (sizeof(s_rlist_btns) / sizeof(s_rlist_btns[0]))

/* 导航提醒页 */
static const btn_def_t s_nav_btns[] = {
    { UI_BTN_NAV_TEST,     6, 176, 150, 42, "测试提醒", COL_WARN },
    { UI_BTN_HOME,       164, 176, 150, 42, "首页",     COL_SUBTEXT },
};

#define NAV_BTN_CNT (sizeof(s_nav_btns) / sizeof(s_nav_btns[0]))

/* 升级页 */
static const btn_def_t s_ota_btns[] = {
    { UI_BTN_OTA_START,    6, 170, 150, 44, "开始升级", COL_ACCENT },
    { UI_BTN_HOME,       164, 170, 150, 44, "首页",     COL_SUBTEXT },
};

#define OTA_BTN_CNT (sizeof(s_ota_btns) / sizeof(s_ota_btns[0]))

/* 校准模式按钮：两个大按钮 + 一个完成。
   完成按钮必须完整落在底栏(218)之上，之前 h=80 时会被 footer 压住一半。 */
#define CAL_BTN_Y      110
#define CAL_BTN_H      64

static const btn_def_t s_cal_btns[] = {
    { UI_BTN_CAL_NEXT,   6,  CAL_BTN_Y, 150, CAL_BTN_H, "校准下一集", COL_ACCENT },
    { UI_BTN_CAL_PREV, 164,  CAL_BTN_Y, 150, CAL_BTN_H, "校准上一集", COL_ACCENT },
    { UI_BTN_CAL_EXIT, 80,  CAL_BTN_Y + CAL_BTN_H + 8, 160, 32, "完成", COL_OK },
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

/* ---- 首页轮播 ----
 * 一屏一个 icon，左右滑动切换；点 icon 进对应功能页。
 * 页序 = home_icon_t 枚举顺序。 */
static int s_home_page;

static const struct {
    home_icon_t icon;
    const char *name;     /* icon 下方标题 */
    const char *glyph;    /* icon 方块里的 2 字大字 */
    uint16_t color;
} s_home_icons[HOME_ICON_COUNT] = {
    { HOME_ICON_BILI,  "B站视频控制", "B站", COL_ACCENT },
    { HOME_ICON_MOUSE, "鼠标模拟",     "鼠标", COL_OK },
    { HOME_ICON_IR,    "红外模拟",     "红外", COL_WARN },
    { HOME_ICON_RADIO, "网络电台",     "电台", 0x4BD7 /* 亮青 */ },
    { HOME_ICON_NAV,   "导航提醒",     "导航", 0xFD20 /* 橙 */ },
    { HOME_ICON_OTA,   "在线升级",     "升级", COL_SUBTEXT },
    { HOME_ICON_CAL,   "触摸校准",     "校准", 0xFB56 /* 粉 */ },
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

static void fill_round_rect(int x, int y, int w, int h, int r, uint16_t color)
{
    fill_rect(x + r, y, w - 2 * r, h, color);
    fill_rect(x, y + r, r, h - 2 * r, color);
    fill_rect(x + w - r, y + r, r, h - 2 * r, color);
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

/** 顶部状态栏：连接状态点 + 模式文字。
    非首页模式左侧画「‹」，点击头部区域返回首页。 */
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
    fill_round_rect(8, 8, 10, 10, 3, dot_col);

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
    case UI_MODE_NAV:        mode_txt = "导航提醒";   break;
    case UI_MODE_OTA_PAGE:   mode_txt = "在线升级";   break;
    default:                 mode_txt = "";           break;
    }

    int conn_tw;
    const char *conn_txt;
    switch (s_conn) {
    case UI_CONN_CONNECTED:    conn_txt = "已连接"; break;
    case UI_CONN_PAIRING:      conn_txt = "配对中"; break;
    default:                   conn_txt = "未连接"; break;
    }
    conn_tw = text_width(conn_txt);
    draw_text(SCR_W - 10 - conn_tw, 5, conn_txt, dot_col, 100);

    if (s_mode != UI_MODE_HOME) {
        /* 左上角「<」= 点这里回首页；状态点和模式文字整体右移 */
        draw_text(2, 5, "<", COL_ACCENT, 20);
        fill_round_rect(22, 8, 10, 10, 3, dot_col);
        draw_text(38, 5, mode_txt, COL_TEXT, 150);
    } else {
        fill_round_rect(8, 8, 10, 10, 3, dot_col);
        draw_text(24, 5, mode_txt, COL_TEXT, 150);
    }
}

/** 动作卡片：最近一次动作（可指定横向范围，鼠标模式要避开左右键） */
static void draw_action_card_in(int card_x, int card_w)
{
    int card_y = HEADER_H + 4;
    int card_h = 58;

    fill_round_rect(card_x, card_y, card_w, card_h, 8, COL_PANEL);
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
    draw_action_card_in(6, SCR_W - 12);
}

/** 带文字的按钮 */
static void draw_button(int x, int y, int w, int h, const char *label, uint16_t accent)
{
    fill_round_rect(x, y, w, h, 8, COL_PANEL);
    /* 上下强调条，让按钮边界清晰可辨 */
    fill_rect(x + 8, y, w - 16, 3, accent);
    fill_rect(x + 8, y + h - 3, w - 16, 3, accent);

    int tw = text_width(label);
    /* 44px 的小按钮放"音量-"(40px) 时只剩 2px 边距：居中并放宽裁剪，
       代替以前的"超宽就左对齐截断"策略 */
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

/* ==================== 首页轮播 ==================== */

#define HOME_BOX_X   105
#define HOME_BOX_Y   54
#define HOME_BOX_SZ  110

static void draw_home(void)
{
    const char *name = s_home_icons[s_home_page].name;
    const char *glyph = s_home_icons[s_home_page].glyph;
    uint16_t col = s_home_icons[s_home_page].color;

    /* 大 icon 方块 + 描边 */
    fill_round_rect(HOME_BOX_X, HOME_BOX_Y, HOME_BOX_SZ, HOME_BOX_SZ, 14, COL_PANEL);
    draw_frame(HOME_BOX_X, HOME_BOX_Y, HOME_BOX_SZ, HOME_BOX_SZ, 3, col);
    int gw = text_width(glyph);
    draw_text(HOME_BOX_X + (HOME_BOX_SZ - gw) / 2, HOME_BOX_Y + 18, glyph, col, HOME_BOX_SZ - 8);
    /* 「点按打开」小字 */
    int hw = text_width("点按打开");
    draw_text(HOME_BOX_X + (HOME_BOX_SZ - hw) / 2, HOME_BOX_Y + 80, "点按打开", COL_SUBTEXT, HOME_BOX_SZ - 8);

    /* 功能名 */
    int nw = text_width(name);
    draw_text((SCR_W - nw) / 2, HOME_BOX_Y + HOME_BOX_SZ + 12, name, COL_TEXT, 200);

    /* 页点 */
    for (int i = 0; i < HOME_ICON_COUNT; i++) {
        int dx = (SCR_W - HOME_ICON_COUNT * 14) / 2 + i * 14 + 4;
        fill_round_rect(dx, 208, 6, 6, 3, i == s_home_page ? COL_ACCENT : COL_SUBTEXT);
    }

    /* 左右滑动提示 */
    draw_text(12, 100, "<", COL_SUBTEXT, 20);
    draw_text(SCR_W - 24, 100, ">", COL_SUBTEXT, 20);
}

/* ==================== 电台页（背景图版） ==================== */

static void draw_radio_bg(void)
{
    /* 逐行拷贝背景（PSRAM 源 -> 帧缓冲），比逐像素快 */
    for (int y = 0; y < SCR_H; y++) {
        memcpy(s_buf + y * SCR_W, bg_radio + y * SCR_W, SCR_W * sizeof(uint16_t));
    }
}

static void draw_radio_list(void)
{
    /* 列表占满内容区，不画动作卡片（否则和行文字重叠） */
    int cnt = radio_station_count();
    if (cnt <= 0) {
        draw_text(60, 110, "台列表为空，回到电台页重进", COL_SUBTEXT, 220);
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

/* ==================== 导航提醒页 ==================== */

static void draw_nav_page(void)
{
    char msg[NAV_TEXT_MAX];
    nav_alert_get(msg, sizeof(msg), NULL);
    if (!msg[0]) {
        strcpy(msg, "暂无提醒");
    }

    /* 大卡片：最近一条提醒 */
    fill_round_rect(6, 32, 308, 76, 8, COL_PANEL);
    fill_rect(6, 32, 308, 3, COL_WARN);
    int mw = text_width(msg);
    draw_text(mw > 292 ? 14 : (6 + (308 - mw) / 2), 44, msg, COL_TEXT, 292);
    int64_t ts = 0;
    nav_alert_get(msg, sizeof(msg), &ts);
    if (ts) {
        char ago[32];
        snprintf(ago, sizeof(ago), "%d 分钟前", (int)((esp_timer_get_time() - ts) / 60000000ll));
        int aw = text_width(ago);
        draw_text((320 - aw) / 2, 84, ago, COL_SUBTEXT, 160);
    }

    /* 推送地址说明 */
    char ip[24];
    nav_alert_get_ip(ip, sizeof(ip));
    fill_round_rect(6, 116, 308, 52, 8, COL_PANEL);
    draw_text(14, 122, "iPhone 快捷指令推送到:", COL_SUBTEXT, 290);
    char line[64];
    snprintf(line, sizeof(line), "http://%s:8080/nav?text=内容", ip);
    draw_text(14, 142, line, COL_TEXT, 292);

    draw_btn_group(s_nav_btns, NAV_BTN_CNT);
}

/* ==================== 升级页 ==================== */

static void draw_ota_page(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ota_progress_t p;
    ota_update_get_progress(&p);

    fill_round_rect(6, 32, 308, 130, 8, COL_PANEL);

    char line[96];
    snprintf(line, sizeof(line), "当前版本: %.40s", app->version);
    draw_text(16, 42, line, COL_TEXT, 290);

    snprintf(line, sizeof(line), "状态: %.24s %.40s", p.stage, p.detail);
    draw_text(16, 64, line, strcmp(p.stage, "失败") == 0 ? COL_ERR : COL_SUBTEXT, 290);

    /* 进度条 */
    draw_frame(16, 92, 288, 18, 2, COL_ACCENT);
    if (p.percent > 0) {
        fill_rect(18, 94, (int)(284 * (p.percent < 100 ? p.percent : 100)) / 100, 14, COL_ACCENT);
    }
    if (p.percent >= 0) {
        snprintf(line, sizeof(line), "%d%%", p.percent);
    } else {
        strlcpy(line, "--%", sizeof(line));
    }
    draw_text(148, 114, line, COL_TEXT, 60);

    if (p.total_kb > 0) {
        snprintf(line, sizeof(line), "升级包: %d KB / 已下载 %d KB",
                 p.total_kb, p.done_kb);
        draw_text(16, 136, line, COL_SUBTEXT, 290);
    } else {
        draw_text(16, 136, "升级包大小在检查更新后显示", COL_SUBTEXT, 290);
    }

    draw_btn_group(s_ota_btns, OTA_BTN_CNT);
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
    } else {
        fill_rect(0, 0, SCR_W, SCR_H, COL_BG);
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
        draw_action_card();
        draw_btn_group(s_radio_btns, RADIO_BTN_CNT);
        break;
    case UI_MODE_RADIO_LIST:
        draw_radio_list();
        draw_btn_group(s_rlist_btns, RLIST_BTN_CNT);
        break;
    case UI_MODE_NAV:
        draw_nav_page();
        break;
    case UI_MODE_OTA_PAGE:
        draw_ota_page();
        break;
    default:
        /* 鼠标模式：左右两个按住型按键，中间是触控板。
           卡片缩窄到中间，避免被按键压住。 */
        draw_action_card_in(70, SCR_W - 140);
        draw_btn_group(s_mouse_btns, MOUSE_BTN_CNT);
        break;
    }

    if (s_mode != UI_MODE_RADIO && s_mode != UI_MODE_RADIO_LIST) {
        draw_footer();
    }

    esp_lcd_panel_draw_bitmap(s_lcd.panel, 0, 0, SCR_W, SCR_H, s_buf);
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
        /* 大 icon 热区（略放大，好点） */
        if (x >= HOME_BOX_X - 10 && x < HOME_BOX_X + HOME_BOX_SZ + 10 &&
            y >= HOME_BOX_Y - 6 && y < HOME_BOX_Y + HOME_BOX_SZ + 40) {
            return UI_BTN_HOME_ICON0;
        }
        if (x < 60) {
            return UI_BTN_HOME_PREV;
        }
        if (x >= SCR_W - 60) {
            return UI_BTN_HOME_NEXT;
        }
        return UI_BTN_NONE;
    }
    /* 非首页：点顶部状态栏 = 返回首页 */
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
        return hit_group(s_rlist_btns, RLIST_BTN_CNT, x, y);
    }
    if (s_mode == UI_MODE_NAV) {
        return hit_group(s_nav_btns, NAV_BTN_CNT, x, y);
    }
    if (s_mode == UI_MODE_OTA_PAGE) {
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
    strcpy(s_footer, "左右滑动选功能，点 icon 打开");

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
