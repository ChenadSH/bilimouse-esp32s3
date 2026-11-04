#!/usr/bin/env python3
"""
离线预览屏上界面。

直接读 main/ui/ui_font.c 里的点阵数据来渲染，
所以看到的就是设备上真实显示的样子（同样的字形、同样的像素）。
用来检查两件事：
  1. 有没有渲染成 '?' 的缺字
  2. 按钮文字会不会超出按钮宽度

用法: python tools/preview_ui.py [out.png]
"""
import os
import re
import sys

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
FONT_C = os.path.join(PROJECT, "main", "ui", "ui_font.c")
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(PROJECT, "build", "ui_preview.png")

SCR_W, SCR_H = 320, 240
STRIDE = 33

COL_BG = 0x0861
COL_PANEL = 0x10E3
COL_ACCENT = 0x2D7F
COL_TEXT = 0xFFFF
COL_SUBTEXT = 0x8410
COL_OK = 0x07E0
COL_WARN = 0xFFE0
COL_ERR = 0xF800


def rgb565(c):
    return (((c >> 11) & 0x1F) * 255 // 31,
            ((c >> 5) & 0x3F) * 255 // 63,
            (c & 0x1F) * 255 // 31)


def parse_arrays():
    src = open(FONT_C, encoding="utf-8").read()

    def grab(name):
        m = re.search(r"static const uint8_t %s\[\]\[%d\] = \{(.*?)\n\};" % (name, STRIDE),
                      src, re.S)
        rows = []
        for body in re.findall(r"\{([\d,\s]+)\}", m.group(1)):
            vals = [int(x) for x in body.split(",") if x.strip() != ""]
            rows.append(vals[:STRIDE])
        return rows

    m = re.search(r"s_cjk_codepoints\[[^\]]*\]\s*=\s*\{([^}]*)\}", src)
    cps = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(1))]
    cjk = dict(zip(cps, grab("s_cjk_font")))
    ascii_tbl = grab("s_ascii_font")
    return cjk, ascii_tbl


def main():
    cjk, ascii_tbl = parse_arrays()
    img = Image.new("RGB", (SCR_W, SCR_H), rgb565(COL_BG))
    px = img.load()

    def set_px(x, y, col):
        if 0 <= x < SCR_W and 0 <= y < SCR_H:
            px[x, y] = col

    def fill(x, y, w, h, col):
        c = rgb565(col)
        for j in range(y, y + h):
            for i in range(x, x + w):
                set_px(i, j, c)

    def round_rect(x, y, w, h, r, col):
        fill(x + r, y, w - 2 * r, h, col)
        fill(x, y + r, r, h - 2 * r, col)
        fill(x + w - r, y + r, r, h - 2 * r, col)

    missing = []

    def glyph(ch, col):
        cp = ord(ch)
        if 0x20 <= cp <= 0x7E:
            g = ascii_tbl[cp - 0x20]
            return g, 8
        g = cjk.get(cp)
        if g is None:
            missing.append(ch)
            g = ascii_tbl[ord("?") - 0x20]
            return g, 8
        return g, 16

    def text(x, y, s, col):
        cx = x
        for ch in s:
            g, w = glyph(ch, col)
            c = rgb565(col)
            bpr = g[0]
            for gy in range(16):
                for gx in range(w):
                    byte = gx // 8
                    if byte >= bpr:
                        continue
                    if g[1 + gy * bpr + byte] & (1 << (7 - gx % 8)):
                        set_px(cx + gx, y + gy, c)
            cx += w
        return cx - x

    def tw(s):
        return sum(8 if 0x20 <= ord(c) <= 0x7E else 16 for c in s)

    def button(x, y, w, h, label, accent):
        round_rect(x, y, w, h, 8, COL_PANEL)
        fill(x + 8, y, w - 16, 3, accent)
        fill(x + 8, y + h - 3, w - 16, 3, accent)
        t = tw(label)
        tx = x + (w - t) // 2
        if t > w - 8:
            print("[overflow] '%s' %dpx > button %dpx" % (label, t, w - 8))
            tx = x + 4
        text(tx, y + (h - 16) // 2, label, COL_TEXT)

    # 顶栏
    fill(0, 0, SCR_W, 26, COL_PANEL)
    round_rect(8, 8, 10, 10, 3, COL_OK)
    text(24, 5, "遥控模式", COL_TEXT)
    text(SCR_W - 10 - tw("已连接"), 5, "已连接", COL_OK)

    # 动作卡片
    round_rect(6, 30, SCR_W - 12, 58, 8, COL_PANEL)
    fill(6, 30, SCR_W - 12, 3, COL_ACCENT)
    title, sub = "已连接", "点屏幕按钮即可操作"
    text((SCR_W - tw(title)) // 2, 40, title, COL_TEXT)
    text((SCR_W - tw(sub)) // 2, 62, sub, COL_SUBTEXT)

    # 按钮
    for b in [(6, 96, 100, 64, "上一集"), (110, 96, 100, 64, "下一集"),
              (214, 96, 100, 64, "播放/暂停")]:
        button(*b, COL_ACCENT)
    for b in [(4, 160, 60, 54, "音量-"), (68, 160, 60, 54, "音量+"),
              (132, 160, 60, 54, "全屏"), (196, 160, 60, 54, "鼠标"),
              (260, 160, 56, 54, "校准")]:
        button(*b, COL_SUBTEXT)

    # 底栏
    fill(0, 218, SCR_W, SCR_H - 218, COL_PANEL)
    text(8, 222, "实体键 = 鼠标左键", COL_SUBTEXT)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    img = img.resize((SCR_W * 2, SCR_H * 2), Image.NEAREST)
    img.save(OUT)
    print("glyphs:", len(cjk), "->", OUT)
    if missing:
        print("MISSING GLYPHS:", "".join(sorted(set(missing))))
    else:
        print("no missing glyphs")


if __name__ == "__main__":
    main()
