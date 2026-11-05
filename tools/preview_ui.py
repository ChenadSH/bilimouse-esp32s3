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


def parse_ui_layout():
    """
    从 main/ui/ui.c 解析按钮布局和宏定义，预览永远跟着真实布局走，
    不会再出现"预览是旧的、屏幕是新的"。
    """
    src = open(os.path.join(PROJECT, "main", "ui", "ui.c"), encoding="utf-8").read()
    # 先剥掉注释，否则 #define 行尾的 /* ... */ 会让整行匹配失败
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)

    defines = {"SCR_W": SCR_W, "SCR_H": SCR_H}
    for m in re.finditer(r"^#define\s+(\w+)\s+([0-9A-Za-z_+\-*/()\s]+)$",
                         src, re.M):
        name, expr = m.group(1), m.group(2).strip()
        try:
            defines[name] = int(eval(expr, {"__builtins__": {}}, dict(defines)))
        except Exception:
            pass

    groups = {}
    for arr, key in [("s_main_btns", "main"), ("s_sub_btns", "sub"),
                     ("s_mouse_btns", "mouse"), ("s_ir_btns", "ir"),
                     ("s_cal_btns", "cal")]:
        m = re.search(re.escape(arr) + r"\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
        if not m:
            continue
        btns = []
        for item in re.findall(r"\{([^{}]+)\}", m.group(1)):
            parts = [p.strip() for p in item.split(",")]
            if len(parts) < 6:
                continue
            try:
                x, y, w, h = (int(eval(p, {"__builtins__": {}}, dict(defines)))
                              for p in parts[1:5])
            except Exception:
                continue
            label = parts[5].strip().strip('"')
            btns.append((x, y, w, h, label))
        groups[key] = btns
    return groups


def main():
    cjk, ascii_tbl = parse_arrays()
    groups = parse_ui_layout()

    screens = [
        ("遥控模式", groups.get("main", []) + groups.get("sub", []),
         "已连接", "点屏幕按钮即可操作", "实体键=空格  轻点=左键"),
        ("鼠标模式", groups.get("mouse", []),
         "鼠标模式", "两侧=左右键", "中间拖动=移动"),
        ("红外遥控", groups.get("ir", []),
         "红外遥控", "槽 1/10 空 已学 0 个", "学习=复制 发射=回放"),
        ("校准模式", groups.get("cal", []),
         "校准模式", "点住红框松手记录", "3 个点，跟着红框走"),
    ]

    sheet = Image.new("RGB", ((SCR_W + 8) * 2, (SCR_H + 8) * 2), (40, 40, 40))
    missing = []
    overflow = []

    for idx, (mode, btns, title, sub, footer) in enumerate(screens):
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

        def glyph(ch):
            cp = ord(ch)
            if 0x20 <= cp <= 0x7E:
                return ascii_tbl[cp - 0x20], 8
            g = cjk.get(cp)
            if g is None:
                missing.append(ch)
                return ascii_tbl[ord("?") - 0x20], 8
            return g, 16

        def text(x, y, s, col):
            cx = x
            for ch in s:
                g, w = glyph(ch)
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
            if tx < x + 2:
                tx = x + 2
            # 与 ui.c draw_button 同步：居中，可用宽度 w-4（超宽截断并报 overflow）
            if t > w - 4:
                overflow.append("%s: '%s' %dpx > %dpx" % (mode, label, t, w - 4))
            stop = tx + (w - 4)
            cx = tx
            for ch in label:
                cw = 8 if 0x20 <= ord(ch) <= 0x7E else 16
                if cx + cw > stop:
                    break
                text(cx, y + (h - 16) // 2, ch, COL_TEXT)
                cx += cw

        # 顶栏
        fill(0, 0, SCR_W, 26, COL_PANEL)
        round_rect(8, 8, 10, 10, 3, COL_OK)
        text(24, 5, mode, COL_TEXT)
        text(SCR_W - 10 - tw("已连接"), 5, "已连接", COL_OK)

        # 动作卡片
        card_x, card_w = (70, SCR_W - 140) if mode == "鼠标模式" else (6, SCR_W - 12)
        round_rect(card_x, 30, card_w, 58, 8, COL_PANEL)
        fill(card_x, 30, card_w, 3, COL_ACCENT)
        text(card_x + (card_w - tw(title)) // 2, 40, title, COL_TEXT)
        text(card_x + (card_w - tw(sub)) // 2, 62, sub, COL_SUBTEXT)

        for b in btns:
            button(b[0], b[1], b[2], b[3], b[4], COL_ACCENT)

        # 底栏
        fill(0, 218, SCR_W, SCR_H - 218, COL_PANEL)
        text(8, 222, footer, COL_SUBTEXT)

        sheet.paste(img, ((idx % 2) * (SCR_W + 8) + 4,
                          (idx // 2) * (SCR_H + 8) + 4))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sheet.save(OUT)
    print("glyphs:", len(cjk), "->", OUT)
    if missing:
        print("MISSING GLYPHS:", "".join(sorted(set(missing))))
    else:
        print("no missing glyphs")
    if overflow:
        print("OVERFLOW:")
        for line in overflow:
            print("  " + line)
    else:
        print("no text overflow")


if __name__ == "__main__":
    main()
