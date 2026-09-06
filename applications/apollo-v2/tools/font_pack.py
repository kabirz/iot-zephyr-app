#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""生成 apollo-v2 QSPI 中文字库 font.bin (AF16 格式, 计划 §10.4)。

字符集: GB2312 一级汉字 3755 + ASCII 0x20-0x7E, 16px 4bpp 反锯齿,
位图 LVGL A4 行打包 (每字节 2 像素, 左像素在高半字节), 基线 y=13。

格式:
  0x00 "AF16" | 0x04 count LE32 | 0x08 index_off LE32 | 0x0C bitmap_off LE32
  码表: {unicode LE32, bitmap_off LE32} x count (unicode 升序)
  位图: 每字 16x16 4bpp = 128B

用法: font_pack.py [NotoSansCJK.ttc] [out=font.bin]
"""

import struct
import sys

from PIL import Image, ImageDraw, ImageFont

FONT_PATH = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"
PX = 16
BASELINE = 13


def load_font():
    """在 ttc 集合中找 Noto Sans CJK SC 面"""
    for idx in range(16):
        try:
            f = ImageFont.truetype(FONT_PATH, PX, index=idx)
        except OSError:
            break
        name = " ".join(f.getname())
        if "SC" in name:
            print(f"字体: {name} (index {idx})")
            return f
    raise SystemExit(f"未在 {FONT_PATH} 找到 SC 字体面")


def charset():
    chars = []
    for row in range(0xB0, 0xD8):		# GB2312 一级: 0xB0A1 - 0xD7FE
        for col in range(0xA1, 0xFF):
            try:
                ch = bytes([row, col]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            chars.append(ch)
    chars.extend(chr(c) for c in range(0x20, 0x7F))
    chars = sorted(set(chars))
    return chars


def render(f, ch):
    """渲染到 16x16 4bpp (128B), 基线 y=13, 水平居中"""
    x0, y0, x1, y1 = f.getbbox(ch)
    ascent, _ = f.getmetrics()
    w, h = x1 - x0, y1 - y0

    img = Image.new("L", (PX, PX), 0)
    draw = ImageDraw.Draw(img)
    dx = (PX - w) // 2 - x0
    dy = BASELINE - ascent - y0
    draw.text((dx, dy), ch, font=f, fill=255)

    out = bytearray()
    px = img.load()
    for y in range(PX):
        for xb in range(PX // 2):
            hi = round(px[xb * 2, y] * 15 / 255)
            lo = round(px[xb * 2 + 1, y] * 15 / 255)
            out.append((hi << 4) | lo)
    return bytes(out)


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else FONT_PATH
    out_path = sys.argv[2] if len(sys.argv) > 2 else "font.bin"

    f = load_font()
    chars = charset()
    print(f"字符数: {len(chars)} (GB2312 一级 + ASCII)")

    bitmaps = b""
    index = []
    for ch in chars:
        index.append((ord(ch), len(bitmaps)))
        bitmaps += render(f, ch)

    count = len(index)
    index_off = 16
    bitmap_off = index_off + count * 8
    with open(out_path, "wb") as fp:
        fp.write(b"AF16")
        fp.write(struct.pack("<III", count, index_off, bitmap_off))
        for cp, off in index:
            fp.write(struct.pack("<II", cp, off))
        fp.write(bitmaps)

    total = bitmap_off + len(bitmaps)
    print(f"已生成 {out_path}: {total} 字节 ({total/1024:.0f} KB), {count} 字")
    print("烧写流程:")
    print("  python tools/smp_udp.py <ip> fswrite font.bin /SD:/font.bin")
    print("  串口: apollo fontinstall && apollo reboot")


if __name__ == "__main__":
    main()
