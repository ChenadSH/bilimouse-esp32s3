#!/usr/bin/env python3
"""
生成 BiliMouse UI 用的中文/ASCII 点阵字库 (C 数组)。

设计取舍:
  - 不用 LVGL / 不引入外部字库 -> 工程零外部依赖，离线可编译
  - 只生成 UI 实际用到的字符 -> 字库极小
  - 16x16 中文 + 8x16 ASCII -> 320x240 屏上排版 comfortable

输出: main/ui/ui_font.c + main/ui/ui_font.h
用法: python tools/gen_font.py
"""
import os
import re
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
OUT_DIR = os.path.join(PROJECT, "main", "ui")
SRC_DIR = os.path.join(PROJECT, "main")

# ---- 字体资源 ----
CJK_FONT = r"C:\Windows\Fonts\msyhbd.ttc"    # 微软雅黑 Bold
ASCII_FONT = r"C:\Windows\Fonts\consola.ttf"  # Consolas

CJK_SIZE = 16
ASCII_SIZE = 16
ASCII_W = 8
CJK_W = 16
GLYPH_H = 16

# 兜底字符集（即使源码扫描漏了，这些也必须有）
CJK_CHARS = (
    "下一集上一集播放暂停音量快进后退倍速全屏弹幕静音"
    "校准完成失败未连接已连接配对中请稍候鼠标模式遥控器体感"
    "灵敏度亮度背光设置保存恢复默认确认取消提示操作指南"
    "设备就绪等待主机识别信号强弱剩余电量开关状态指示当前"
    "动作触发切集选集单多视频网页播放器主菜单返回首页进度时长"
    "已配对无信号蓝牙已断开重连中切集成功"
)

# 源码里出现但字库没有 -> 屏幕上显示成 '?'，这是「一大堆问号」的根因。
# 直接从 main/ 下所有 C 源码的字符串字面量里收集汉字与全角标点，
# 保证「代码里写了什么，字库里就有什么」，不会再漏。
STR_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
CJK_RANGES = (
    (0x4E00, 0x9FFF),   # 常用汉字
    (0x3000, 0x303F),   # 中文标点
    (0xFF00, 0xFFEF),   # 全角字符
)


def collect_chars_from_source():
    chars = set()
    for root, _dirs, files in os.walk(SRC_DIR):
        for name in files:
            # Kconfig.projbuild 里有 WiFi 热点名（如「九天诛仙」），
            # 不扫它屏上显示 SSID 就全是问号
            if not name.endswith((".c", ".h", ".json")) and name != "Kconfig.projbuild":
                continue
            path = os.path.join(root, name)
            if os.path.abspath(path) == os.path.abspath(os.path.join(OUT_DIR, "ui_font.c")):
                continue
            with open(path, encoding="utf-8", errors="ignore") as fh:
                text = fh.read()
            for lit in STR_RE.findall(text):
                for ch in lit:
                    cp = ord(ch)
                    if any(lo <= cp <= hi for lo, hi in CJK_RANGES):
                        chars.add(ch)
    # 仓库根目录的电台台列表 JSON：屏上要显示台名（中文），
    # 台名是运行时从网络拉的，只有扫这个文件才能进字库
    radio_json = os.path.join(PROJECT, "radio", "radio_stations.json")
    if os.path.isfile(radio_json):
        with open(radio_json, encoding="utf-8", errors="ignore") as fh:
            for ch in fh.read():
                cp = ord(ch)
                if any(lo <= cp <= hi for lo, hi in CJK_RANGES):
                    chars.add(ch)
    return chars

# ASCII 全量 0x20~0x7E，保证固件侧可直接下标寻址
ASCII_FIRST = 0x20
ASCII_LAST = 0x7E
ASCII_CHARS = "".join(chr(c) for c in range(ASCII_FIRST, ASCII_LAST + 1))


def render_glyph(font, ch, w, h):
    """渲染单字符为 1bpp 位图，返回 (bytes_per_row, rows list[int])，MSB first"""
    img = Image.new("1", (w, h), 0)
    d = ImageDraw.Draw(img)
    try:
        bbox = d.textbbox((0, 0), ch, font=font)
        tw = bbox[2] - bbox[0]
        th = bbox[3] - bbox[1]
        ox = (w - tw) // 2 - bbox[0]
        oy = (h - th) // 2 - bbox[1]
    except Exception:
        ox = oy = 0
    d.text((ox, oy), ch, fill=1, font=font)
    px = img.load()
    bpr = (w + 7) // 8
    rows = []
    for y in range(h):
        row_bytes = [0] * bpr
        for x in range(w):
            if px[x, y]:
                byte_idx = x // 8
                bit_idx = 7 - (x % 8)
                row_bytes[byte_idx] |= (1 << bit_idx)
        rows.extend(row_bytes)
    return bpr, rows


def build_table(chars, font, w, h):
    table = {}
    for ch in sorted(set(chars)):
        table[ch] = render_glyph(font, ch, w, h)
    return table


def emit_c_array(fh, name, table, order):
    """输出 C 数组: 每个字符 {bpr, bytes[bpr*16]}，bytes 为 MSB-first 逐行排列"""
    stride = 1 + GLYPH_H * 2   # 最大跨度 (16px 宽 -> 2 字节/行)
    fh.write(f"static const uint8_t {name}[][{stride}] = {{\n")
    for ch in order:
        if ch not in table:
            continue
        bpr, rows = table[ch]
        data = bytes([bpr] + rows)
        data = data + bytes(stride - len(data))   # 补齐到统一跨度
        esc = ",".join(str(b) for b in data)
        fh.write(f"    {{{esc}}},  /* U+{ord(ch):04X} '{ch}' */\n")
    fh.write("};\n\n")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    cjk_font = ImageFont.truetype(CJK_FONT, CJK_SIZE)
    ascii_font = ImageFont.truetype(ASCII_FONT, ASCII_SIZE)

    cjk_set = set(CJK_CHARS) | collect_chars_from_source()
    # 空格之类的不可见字符不需要点阵
    cjk_set = {c for c in cjk_set if not c.isspace()}
    print(f"[gen_font] collected {len(cjk_set)} CJK chars from source")

    cjk_table = build_table(cjk_set, cjk_font, CJK_W, CJK_SIZE)
    ascii_table = build_table(ASCII_CHARS, ascii_font, ASCII_W, ASCII_SIZE)

    cjk_order = sorted(cjk_table.keys(), key=ord)
    ascii_order = sorted(ascii_table.keys(), key=ord)

    # 校验 ASCII 连续性（固件侧按下标直接寻址，必须连续无空洞）
    assert ascii_order == [chr(c) for c in range(ASCII_FIRST, ASCII_LAST + 1)], "ASCII table must be contiguous"

    hpath = os.path.join(OUT_DIR, "ui_font.h")
    cpath = os.path.join(OUT_DIR, "ui_font.c")

    with open(hpath, "w", encoding="utf-8") as fh:
        fh.write("""/**
 * @file ui_font.h
 * @brief BiliMouse UI 点阵字库 (自动生成，请勿手改)
 *
 * 由 tools/gen_font.py 生成：
 *   - 中文 16x16 (微软雅黑 Bold)
 *   - ASCII 8x16 (Consolas)
 *
 * 存储格式: 每个字符 {bpr, row[16]}，row 为 MSB-first 位图。
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UI_FONT_CJK_W       %d
#define UI_FONT_CJK_H       %d
#define UI_FONT_ASCII_W     %d
#define UI_FONT_ASCII_H     %d

#define UI_FONT_CJK_COUNT   %d
#define UI_FONT_ASCII_COUNT %d
#define UI_FONT_ASCII_FIRST 0x%02X
#define UI_FONT_ASCII_LAST  0x%02X

/** 查表：返回字符位图指针 (bpr + rows)，未找到返回 NULL */
const uint8_t *ui_font_get_cjk(uint16_t codepoint);
const uint8_t *ui_font_get_ascii(uint8_t ch);

#ifdef __cplusplus
}
#endif
""" % (CJK_W, GLYPH_H, ASCII_W, GLYPH_H, len(cjk_table), len(ascii_table),
       ASCII_FIRST, ASCII_LAST))

    with open(cpath, "w", encoding="utf-8") as fh:
        fh.write("""/**
 * @file ui_font.c
 * @brief BiliMouse UI 点阵字库 (自动生成，请勿手改)
 */
#include "ui_font.h"

#include <stddef.h>

""")
        emit_c_array(fh, "s_cjk_font", cjk_table, cjk_order)
        emit_c_array(fh, "s_ascii_font", ascii_table, ascii_order)

        fh.write("const uint8_t *ui_font_get_cjk(uint16_t codepoint)\n{\n")
        fh.write("    static const uint16_t s_cjk_codepoints[UI_FONT_CJK_COUNT] = {\n")
        fh.write("        " + ", ".join("0x%04X" % ord(c) for c in cjk_order) + "\n")
        fh.write("    };\n\n")
        fh.write("    for (int i = 0; i < UI_FONT_CJK_COUNT; i++) {\n")
        fh.write("        if (s_cjk_codepoints[i] == codepoint) {\n")
        fh.write("            return s_cjk_font[i];\n")
        fh.write("        }\n")
        fh.write("    }\n")
        fh.write("    return NULL;\n")
        fh.write("}\n\n")

        fh.write("const uint8_t *ui_font_get_ascii(uint8_t ch)\n{\n")
        fh.write(f"    if (ch < UI_FONT_ASCII_FIRST || ch > UI_FONT_ASCII_LAST) {{\n")
        fh.write("        return NULL;\n")
        fh.write("    }\n")
        fh.write("    return s_ascii_font[ch - UI_FONT_ASCII_FIRST];\n")
        fh.write("}\n")

    print(f"[gen_font] CJK: {len(cjk_table)} chars")
    print(f"[gen_font] ASCII: {len(ascii_table)} chars")
    print(f"[gen_font] -> {cpath} ({os.path.getsize(cpath)} bytes)")
    print(f"[gen_font] -> {hpath}")


if __name__ == "__main__":
    main()
