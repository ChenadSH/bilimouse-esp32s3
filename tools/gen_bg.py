"""生成电台页背景：任意图片 -> 320x240 RGB565 C 数组 (main/ui/bg_radio.c)

用法:
  python tools/gen_bg.py <图片路径>
依赖: pillow (pip install pillow)
"""
import sys
import os
from PIL import Image

TW, TH = 320, 240
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def convert(src):
    img = Image.open(src).convert("RGB")
    tr = TW / TH
    w, h = img.size
    r = w / h
    if r > tr:
        nw = int(h * tr)
        x0 = (w - nw) // 2
        img = img.crop((x0, 0, x0 + nw, h))
    else:
        nh = int(w / tr)
        y0 = (h - nh) // 2
        img = img.crop((0, y0, w, y0 + nh))
    img = img.resize((TW, TH), Image.LANCZOS)
    # 轻微压暗（0.95 -> 0.80）：IPS 屏亮度高，原图直出会太刺眼；
    # 控制条与卡片在固件里用半透明面板叠上去，不需要在这里压死。
    px = img.load()
    for y in range(TH):
        f = 0.95 - 0.15 * y / (TH - 1)
        for x in range(TW):
            r0, g0, b0 = px[x, y]
            px[x, y] = (int(r0 * f), int(g0 * f), int(b0 * f))

    # RGB565 值（与帧缓冲同格式；面板 io 是 big-endian，驱动负责字节序）
    out = []
    for y in range(TH):
        for x in range(TW):
            r0, g0, b0 = px[x, y]
            v = ((r0 & 0xF8) << 8) | ((g0 & 0xFC) << 3) | (b0 >> 3)
            out.append(v)

    hdr = os.path.join(ROOT, "main", "ui", "bg_radio.h")
    src_c = os.path.join(ROOT, "main", "ui", "bg_radio.c")
    with open(hdr, "w", encoding="utf-8") as f:
        f.write(
            "/**\n * @file bg_radio.h\n"
            " * @brief 电台页面二次元背景图（320x240 RGB565，tools/gen_bg.py 生成）\n */\n"
            "#pragma once\n\n#include <stdint.h>\n\n"
            "#define BG_RADIO_W 320\n#define BG_RADIO_H 240\n\n"
            "extern const uint16_t bg_radio[BG_RADIO_W * BG_RADIO_H];\n")
    lines = []
    for i in range(0, len(out), 12):
        lines.append("    " + ",".join(f"0x{v:04X}" for v in out[i:i + 12]) + ",")
    with open(src_c, "w", encoding="utf-8") as f:
        f.write("/* 自动生成：电台页背景，320x240 RGB565（与帧缓冲同格式）。生成: python tools/gen_bg.py */\n"
                '#include "bg_radio.h"\n\n'
                '#include "esp_attr.h"\n\n'
                "const uint16_t bg_radio[BG_RADIO_W * BG_RADIO_H] = {\n"
                + "\n".join(lines) + "\n};\n")
    print("written:", src_c, f"({len(out)//1024} KB)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: python tools/gen_bg.py <image>")
    convert(sys.argv[1])
