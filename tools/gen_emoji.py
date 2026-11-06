"""生成首页 icon 的彩色 emoji 位图 (main/ui/ui_emoji.c)

为什么不用点阵字库：
  点阵字库是 1bpp 单色，emoji 是彩色字形（Segoe UI Emoji / COLR 表）。
  这里用 PIL 在 PC 上把 emoji 渲染成 RGBA，再按「icon 磁贴底色」预合成，
  固件端就只需要 RGB565 一种格式，直接整块贴上去，不用做 alpha 混合。

预合成的意义：
  磁贴内部是纯色，emoji 的透明/半透明区域直接变成磁贴底色，
  画的时候与磁贴整块铺过去，视觉上完全一致，但省掉一份 alpha 平面。

输出:
  main/ui/ui_emoji.c  - const uint16_t ui_emoji_xxx[UI_EMOJI_SZ*UI_EMOJI_SZ] (RGB565)
                        + 每个 icon 的磁贴底色 / 描边色

用法:
  python tools/gen_emoji.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
OUT_DIR = os.path.join(PROJECT, "main", "ui")

EMOJI_FONT = r"C:\Windows\Fonts\seguiemj.ttf"   # Segoe UI Emoji（彩色 COLR 字形）
SIZE = 72          # 输出边长（像素）：首页磁贴 96，emoji 贴 72 居中，四周留磁贴底色

# icon 顺序 = 首页轮播顺序
#   name, emoji, 磁贴底色(RGB), 描边色(RGB)
ICONS = [
    ("bili",  "\U0001F4FA", (18, 38, 74),  (86, 156, 255)),   # 📺 B站视频控制
    ("mouse", "\U0001F5B1", (14, 52, 44),  (60, 200, 150)),   # 🖱 鼠标模拟
    ("ir",    "\U0001F4E1", (68, 26, 42),  (255, 120, 150)),  # 📡 红外模拟
    ("radio", "\U0001F3B5", (44, 26, 74),  (170, 130, 255)),  # 🎵 网络电台
    ("ota",   "\U0001F504", (16, 54, 66),  (70, 200, 230)),   # 🔄 在线升级
    ("cal",   "\U0001F3AF", (66, 42, 16),  (250, 180, 70)),   # 🎯 触摸校准
]


def rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def render(ch, tile_bg):
    """渲染单个 emoji -> (SIZE*SIZE) RGB565 列表（已按磁贴底色预合成）"""
    font = ImageFont.truetype(EMOJI_FONT, SIZE)
    img = Image.new("RGBA", (SIZE * 2, SIZE * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.text((SIZE // 2, SIZE // 2), ch, font=font, embedded_color=True)
    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)
    # 等比缩放到 SIZE 内并居中
    scale = min(SIZE / img.width, SIZE / img.height)
    nw, nh = max(1, int(img.width * scale)), max(1, int(img.height * scale))
    img = img.resize((nw, nh), Image.LANCZOS)

    out = Image.new("RGB", (SIZE, SIZE), tile_bg)
    out.paste(img, ((SIZE - nw) // 2, (SIZE - nh) // 2), img)
    px = out.load()
    return [rgb565(*px[x, y]) for y in range(SIZE) for x in range(SIZE)]


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if not os.path.exists(EMOJI_FONT):
        raise SystemExit(f"emoji font not found: {EMOJI_FONT}")

    c_path = os.path.join(OUT_DIR, "ui_emoji.c")
    with open(c_path, "w", encoding="utf-8") as fh:
        fh.write("/* 自动生成：首页 icon 的彩色 emoji 位图。生成: python tools/gen_emoji.py */\n")
        fh.write('#include "ui_emoji.h"\n\n')

        for name, ch, tile, border in ICONS:
            vals = render(ch, tile)
            lines = []
            for i in range(0, len(vals), 8):
                lines.append("    " + ",".join(f"0x{v:04X}" for v in vals[i:i + 8]) + ",")
            fh.write(f"const uint16_t ui_emoji_{name}[UI_EMOJI_SZ * UI_EMOJI_SZ] = {{\n")
            fh.write("\n".join(lines) + "\n};\n\n")
            print(f"  {name}: {ch}")

        # 磁贴配色表（固件画磁贴底色/描边时用）
        fh.write("const uint16_t ui_emoji_tile_bg[UI_EMOJI_COUNT] = {\n")
        for name, ch, tile, border in ICONS:
            fh.write(f"    0x{rgb565(*tile):04X},   /* {name} */\n")
        fh.write("};\n\n")
        fh.write("const uint16_t ui_emoji_tile_border[UI_EMOJI_COUNT] = {\n")
        for name, ch, tile, border in ICONS:
            fh.write(f"    0x{rgb565(*border):04X},   /* {name} */\n")
        fh.write("};\n\n")

    hdr = os.path.join(OUT_DIR, "ui_emoji.h")
    with open(hdr, "w", encoding="utf-8") as fh:
        fh.write("""/**
 * @file ui_emoji.h
 * @brief 首页 icon 的彩色 emoji 位图（自动生成，请勿手改）
 *
 * 由 tools/gen_emoji.py 生成：Segoe UI Emoji 彩色字形 -> RGB565，
 * 已按磁贴底色预合成，固件端直接贴图即可（无需 alpha 混合）。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_EMOJI_SZ 72

/** 首页 icon 顺序（= 首页轮播顺序） */
typedef enum {
    UI_EMOJI_BILI = 0,   /* 📺 B站视频控制 */
    UI_EMOJI_MOUSE,      /* 🖱 鼠标模拟 */
    UI_EMOJI_IR,         /* 📡 红外模拟 */
    UI_EMOJI_RADIO,      /* 🎵 网络电台 */
    UI_EMOJI_OTA,        /* 🔄 在线升级 */
    UI_EMOJI_CAL,        /* 🎯 触摸校准 */
    UI_EMOJI_COUNT
} ui_emoji_t;

extern const uint16_t ui_emoji_bili[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_mouse[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_ir[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_radio[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_ota[UI_EMOJI_SZ * UI_EMOJI_SZ];
extern const uint16_t ui_emoji_cal[UI_EMOJI_SZ * UI_EMOJI_SZ];

/** 每个 icon 的磁贴底色 / 描边色（RGB565） */
extern const uint16_t ui_emoji_tile_bg[UI_EMOJI_COUNT];
extern const uint16_t ui_emoji_tile_border[UI_EMOJI_COUNT];

#ifdef __cplusplus
}
#endif
""")
    print(f"-> {c_path}")
    print(f"-> {hdr}")
    print(f"[gen_emoji] {len(ICONS)} icons, {SIZE}x{SIZE} RGB565, "
          f"{(len(ICONS) * SIZE * SIZE * 2) // 1024} KB")


if __name__ == "__main__":
    main()
