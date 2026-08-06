#!/usr/bin/env python3
"""
SH1106 OLED 显示屏字体库生成工具

生成兼容 SH1106 的字体数组，支持：
  - 内置标准 5x8 ASCII 字体
  - 从 TTF/OTF 字体文件渲染生成任意尺寸位图
  - C 语言二维数组输出

用法:
    python sh1106_font_generator.py [选项]

示例:
    # 使用内置 5x8 字体，输出二维 C 数组
    python sh1106_font_generator.py

    # 仅生成数字 0-9
    python sh1106_font_generator.py --start 48 --end 57 --name font_digits

    # 从 TTF 字体渲染 5x8 位图
    python sh1106_font_generator.py --ttf /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf --size 8

    # 从 TTF 渲染并指定输出宽高（自动缩放）
    python sh1106_font_generator.py --ttf font.ttf --width 5 --height 8 --size 8

    # 预览字符点阵
    python sh1106_font_generator.py --preview 65 66 67
"""

import argparse
import os
import sys

# 标准 5x8 ASCII 字体位图数据（内置备用）
# 每个字符 5 字节，字节内 LSB 为顶部行
FONT_5X8 = {
    0x20: [0x00, 0x00, 0x00, 0x00, 0x00],
    0x21: [0x00, 0x00, 0x5F, 0x00, 0x00],
    0x22: [0x00, 0x07, 0x00, 0x07, 0x00],
    0x23: [0x14, 0x7F, 0x14, 0x7F, 0x14],
    0x24: [0x24, 0x2A, 0x7F, 0x2A, 0x12],
    0x25: [0x23, 0x13, 0x08, 0x64, 0x62],
    0x26: [0x36, 0x49, 0x55, 0x22, 0x50],
    0x27: [0x00, 0x05, 0x03, 0x00, 0x00],
    0x28: [0x00, 0x1C, 0x22, 0x41, 0x00],
    0x29: [0x00, 0x41, 0x22, 0x1C, 0x00],
    0x2A: [0x14, 0x08, 0x3E, 0x08, 0x14],
    0x2B: [0x08, 0x08, 0x3E, 0x08, 0x08],
    0x2C: [0x00, 0x50, 0x30, 0x00, 0x00],
    0x2D: [0x08, 0x08, 0x08, 0x08, 0x08],
    0x2E: [0x00, 0x60, 0x60, 0x00, 0x00],
    0x2F: [0x20, 0x10, 0x08, 0x04, 0x02],
    0x30: [0x3E, 0x51, 0x49, 0x45, 0x3E],
    0x31: [0x00, 0x42, 0x7F, 0x40, 0x00],
    0x32: [0x42, 0x61, 0x51, 0x49, 0x46],
    0x33: [0x21, 0x41, 0x45, 0x4B, 0x31],
    0x34: [0x18, 0x14, 0x12, 0x7F, 0x10],
    0x35: [0x27, 0x45, 0x45, 0x45, 0x39],
    0x36: [0x3C, 0x4A, 0x49, 0x49, 0x30],
    0x37: [0x01, 0x71, 0x09, 0x05, 0x03],
    0x38: [0x36, 0x49, 0x49, 0x49, 0x36],
    0x39: [0x06, 0x49, 0x49, 0x29, 0x1E],
    0x3A: [0x00, 0x36, 0x36, 0x00, 0x00],
    0x3B: [0x00, 0x56, 0x36, 0x00, 0x00],
    0x3C: [0x08, 0x14, 0x22, 0x41, 0x00],
    0x3D: [0x14, 0x14, 0x14, 0x14, 0x14],
    0x3E: [0x00, 0x41, 0x22, 0x14, 0x08],
    0x3F: [0x02, 0x01, 0x51, 0x09, 0x06],
    0x40: [0x32, 0x49, 0x79, 0x41, 0x3E],
    0x41: [0x7E, 0x11, 0x11, 0x11, 0x7E],
    0x42: [0x7F, 0x49, 0x49, 0x49, 0x36],
    0x43: [0x3E, 0x41, 0x41, 0x41, 0x22],
    0x44: [0x7F, 0x41, 0x41, 0x22, 0x1C],
    0x45: [0x7F, 0x49, 0x49, 0x49, 0x41],
    0x46: [0x7F, 0x09, 0x09, 0x09, 0x01],
    0x47: [0x3E, 0x41, 0x49, 0x49, 0x7A],
    0x48: [0x7F, 0x08, 0x08, 0x08, 0x7F],
    0x49: [0x00, 0x41, 0x7F, 0x41, 0x00],
    0x4A: [0x20, 0x40, 0x41, 0x3F, 0x01],
    0x4B: [0x7F, 0x08, 0x14, 0x22, 0x41],
    0x4C: [0x7F, 0x40, 0x40, 0x40, 0x40],
    0x4D: [0x7F, 0x02, 0x0C, 0x02, 0x7F],
    0x4E: [0x7F, 0x04, 0x08, 0x10, 0x7F],
    0x4F: [0x3E, 0x41, 0x41, 0x41, 0x3E],
    0x50: [0x7F, 0x09, 0x09, 0x09, 0x06],
    0x51: [0x3E, 0x41, 0x51, 0x21, 0x5E],
    0x52: [0x7F, 0x09, 0x19, 0x29, 0x46],
    0x53: [0x46, 0x49, 0x49, 0x49, 0x31],
    0x54: [0x01, 0x01, 0x7F, 0x01, 0x01],
    0x55: [0x3F, 0x40, 0x40, 0x40, 0x3F],
    0x56: [0x1F, 0x20, 0x40, 0x20, 0x1F],
    0x57: [0x3F, 0x40, 0x38, 0x40, 0x3F],
    0x58: [0x63, 0x14, 0x08, 0x14, 0x63],
    0x59: [0x07, 0x08, 0x70, 0x08, 0x07],
    0x5A: [0x61, 0x51, 0x49, 0x45, 0x43],
    0x5B: [0x00, 0x7F, 0x41, 0x41, 0x00],
    0x5C: [0x02, 0x04, 0x08, 0x10, 0x20],
    0x5D: [0x00, 0x41, 0x41, 0x7F, 0x00],
    0x5E: [0x04, 0x02, 0x01, 0x02, 0x04],
    0x5F: [0x40, 0x40, 0x40, 0x40, 0x40],
    0x60: [0x00, 0x01, 0x02, 0x04, 0x00],
    0x61: [0x20, 0x54, 0x54, 0x54, 0x78],
    0x62: [0x7F, 0x48, 0x44, 0x44, 0x38],
    0x63: [0x38, 0x44, 0x44, 0x44, 0x20],
    0x64: [0x38, 0x44, 0x44, 0x48, 0x7F],
    0x65: [0x38, 0x54, 0x54, 0x54, 0x18],
    0x66: [0x08, 0x7E, 0x09, 0x01, 0x02],
    0x67: [0x0C, 0x52, 0x52, 0x52, 0x3E],
    0x68: [0x7F, 0x08, 0x04, 0x04, 0x78],
    0x69: [0x00, 0x44, 0x7D, 0x40, 0x00],
    0x6A: [0x20, 0x40, 0x44, 0x3D, 0x00],
    0x6B: [0x7F, 0x10, 0x28, 0x44, 0x00],
    0x6C: [0x00, 0x41, 0x7F, 0x40, 0x00],
    0x6D: [0x7C, 0x04, 0x18, 0x04, 0x78],
    0x6E: [0x7C, 0x08, 0x04, 0x04, 0x78],
    0x6F: [0x38, 0x44, 0x44, 0x44, 0x38],
    0x70: [0x7C, 0x14, 0x14, 0x14, 0x08],
    0x71: [0x08, 0x14, 0x14, 0x18, 0x7C],
    0x72: [0x7C, 0x08, 0x04, 0x04, 0x08],
    0x73: [0x48, 0x54, 0x54, 0x54, 0x20],
    0x74: [0x04, 0x3F, 0x44, 0x40, 0x20],
    0x75: [0x3C, 0x40, 0x40, 0x20, 0x7C],
    0x76: [0x1C, 0x20, 0x40, 0x20, 0x1C],
    0x77: [0x3C, 0x40, 0x30, 0x40, 0x3C],
    0x78: [0x44, 0x28, 0x10, 0x28, 0x44],
    0x79: [0x0C, 0x50, 0x50, 0x50, 0x3C],
    0x7A: [0x44, 0x64, 0x54, 0x4C, 0x44],
    0x7B: [0x00, 0x08, 0x36, 0x41, 0x00],
    0x7C: [0x00, 0x00, 0x7F, 0x00, 0x00],
    0x7D: [0x00, 0x41, 0x36, 0x08, 0x00],
    0x7E: [0x10, 0x08, 0x08, 0x10, 0x08],
}


def render_char_from_ttf(
    font_path: str,
    char: str,
    width: int,
    height: int,
    font_size: int,
    threshold: int = 128,
) -> list[int]:
    """
    从 TTF/OTF 字体文件渲染单个字符为列式位图。

    渲染策略：以 font_size 磅渲染到大画布，裁剪有效区域，
    再缩放到 width x height，最后二值化按列打包。
    每个字节内 LSB = 顶部像素。

    Args:
        font_path: TTF/OTF 字体文件路径
        char: 要渲染的字符
        width: 目标宽度（像素）
        height: 目标高度（像素），必须 <= 8
        font_size: 渲染字号（磅）
        threshold: 二值化阈值（0-255）

    Returns:
        列式位图字节列表，长度为 width
    """
    from PIL import Image, ImageDraw, ImageFont

    if height > 8:
        raise ValueError("高度不能超过 8（SH1106 单页模式）")

    # 以 font_size 为基准构建大画布，保证渲染细节充足
    scale = max(1, font_size // height) * 2
    canvas_w = width * scale + 4
    canvas_h = height * scale + 4
    img = Image.new("L", (canvas_w, canvas_h), 0)
    draw = ImageDraw.Draw(img)

    font = ImageFont.truetype(font_path, font_size)

    # 获取字符边界并居中绘制
    bbox = draw.textbbox((0, 0), char, font=font)
    tw = bbox[2] - bbox[0]
    th = bbox[3] - bbox[1]
    ox = (canvas_w - tw) // 2 - bbox[0]
    oy = (canvas_h - th) // 2 - bbox[1]
    draw.text((ox, oy), char, fill=255, font=font)

    # 裁剪到有效区域（含少许边距）再缩放
    crop = img.getbbox()
    if crop:
        img = img.crop(crop)
    img = img.resize((width, height), Image.LANCZOS)

    # 二值化并按列打包为字节数组
    pixels = img.load()
    bitmap = []
    for col in range(width):
        byte_val = 0
        for row in range(height):
            if pixels[col, row] >= threshold:
                byte_val |= (1 << row)
        bitmap.append(byte_val)

    return bitmap


class SH1106FontGenerator:
    """SH1106 OLED 显示屏字体库生成器"""

    DEFAULT_WIDTH = 5
    DEFAULT_HEIGHT = 8

    def __init__(
        self,
        font_path: str | None = None,
        width: int = 5,
        height: int = 8,
        font_size: int = 8,
    ):
        """
        Args:
            font_path: TTF/OTF 字体文件路径。为 None 时使用内置 5x8 字体。
            width: 字符宽度（像素）
            height: 字符高度（像素），须 <= 8
            font_size: TTF 渲染字号
        """
        if height > 8:
            raise ValueError("高度不能超过 8（SH1106 单页模式，每页 8 行）")

        self.font_path = font_path
        self.width = width
        self.height = height
        self.font_size = font_size

        if font_path:
            self.font_data = {}
            self._source = f"TTF: {os.path.basename(font_path)}"
        else:
            self.font_data = dict(FONT_5X8)
            self._source = "内置 5x8"

    def _get_bitmap(self, code: int) -> list[int]:
        """获取字符位图，TTF 模式下按需渲染并缓存。"""
        if code in self.font_data:
            return self.font_data[code]

        if self.font_path:
            bitmap = render_char_from_ttf(
                self.font_path, chr(code), self.width, self.height, self.font_size,
            )
            self.font_data[code] = bitmap
            return bitmap

        return [0x00] * self.width

    def add_char(self, code: int, bitmap: list[int]):
        """手动添加或覆盖单个字符的位图数据。"""
        if len(bitmap) != self.width:
            raise ValueError(f"位图宽度必须为 {self.width} 字节，当前为 {len(bitmap)}")
        if any(not (0 <= b <= 255) for b in bitmap):
            raise ValueError("位图字节值必须在 0-255 范围内")
        self.font_data[code] = list(bitmap)

    @staticmethod
    def _reverse_bits(b: int) -> int:
        """翻转字节位序: bit0↔bit7, bit1↔bit6, ... (LSB-top → MSB-top)"""
        b = ((b & 0xF0) >> 4) | ((b & 0x0F) << 4)
        b = ((b & 0xCC) >> 2) | ((b & 0x33) << 2)
        b = ((b & 0xAA) >> 1) | ((b & 0x55) << 1)
        return b

    def generate_c_array(
        self,
        name: str = "font_5x8",
        start: int = 0x20,
        end: int = 0x7E,
        stride: int = 0,
    ) -> str:
        """
        生成 C 语言二维数组，格式对齐 font_8x16.c 风格。
        取模方式: 纵向8点上高位 (MSB=top)，SH1106 原生格式。
        stride: 数组每行字节数，0 表示等于 width（无填充）。
        """
        stride = stride if stride > 0 else self.width
        char_count = end - start + 1
        name_upper = name.upper()

        lines = [
            "/*",
            " * Copyright (c) 2026 Kabirz.",
            " * SPDX-License-Identifier: Apache-2.0",
            " *",
            f" * {self.width}x{self.height} ASCII 字体位图",
            " * 取模方式: 纵向8点上高位, 从左到右",
            f" * 每字符 {stride} 字节",
            " * 适用于 SH1106/SSD1306",
            " */",
            "",
            "#include <stdint.h>",
            "",
            "/*",
            f" * 涵盖可打印 ASCII: 0x{start:02X} ('{chr(start) if 0x21 <= start <= 0x7E else ' '}')"
            f" ~ 0x{end:02X} ('{chr(end) if 0x21 <= end <= 0x7E else ' '}')",
            f" * 共 {char_count} 字符, 每字符 {stride} 字节",
            " */",
            f"#define {name_upper}_FIRST 0x{start:02X}",
            f"#define {name_upper}_COUNT {char_count}",
            "",
            f"const uint8_t {name}[{name_upper}_COUNT][{stride}] = {{",
        ]

        for i, code in enumerate(range(start, end + 1)):
            bitmap = self._get_bitmap(code)
            # Pad with 0x00 to reach stride
            padded = bitmap + [0x00] * (stride - len(bitmap))
            hex_str = ",".join(f"0x{b:02X}" for b in padded)
            char_repr = chr(code) if 0x20 <= code <= 0x7E else " "
            comma = "," if i < char_count - 1 else ""
            lines.append(f"\t{{{hex_str}}}{comma}/*\"{char_repr}\",{i}*/")

        lines.append("};")
        return "\n".join(lines)

    def generate_python_dict(
        self,
        name: str = "font_5x8",
        start: int = 0x20,
        end: int = 0x7E,
    ) -> str:
        """生成 Python 字典格式。"""
        lines = []
        char_count = end - start + 1

        lines.append(f"# SH1106 字体库")
        lines.append(f"# 来源: {self._source}")
        lines.append(f"# 字符范围: {start} (0x{start:02X}) ~ {end} (0x{end:02X})")
        lines.append(f"# 每字符: {self.width} 字节 ({self.width}x{self.height})")
        lines.append(f"# 总计: {char_count} 字符")
        lines.append("")
        lines.append(f'{name} = {{')

        for code in range(start, end + 1):
            bitmap = self._get_bitmap(code)
            hex_str = ", ".join(f"0x{b:02X}" for b in bitmap)
            char_repr = chr(code) if 0x21 <= code <= 0x7E else " "
            lines.append(f"    0x{code:02X}: [{hex_str}],  # '{char_repr}'")

        lines.append("}")
        return "\n".join(lines)

    def _to_msb_bitmap(self, bitmap: list[int]) -> list[int]:
        """将内部 LSB-top 位图转为 SH1106 MSB-top 格式。"""
        return [self._reverse_bits(b) for b in bitmap]

    def preview_char(self, code: int) -> str:
        """以文本形式预览字符点阵（MSB-top，与 SH1106 实际显示一致）。"""
        bitmap = self._to_msb_bitmap(self._get_bitmap(code))
        char_repr = chr(code) if 0x21 <= code <= 0x7E else " "
        lines = [f"--- '{char_repr}' (0x{code:02X}) ---"]

        for row in range(self.height):
            pixel_line = ""
            for col in range(self.width):
                bit = (bitmap[col] >> (7 - row)) & 0x01
                pixel_line += "█" if bit else "·"
            lines.append(pixel_line)

        return "\n".join(lines)

    def preview_png(
        self,
        path: str,
        start: int = 0x20,
        end: int = 0x7E,
        scale: int = 8,
        cols: int = 16,
        fg: tuple = (0, 200, 60),
        bg: tuple = (10, 10, 10),
        label_height: int = 12,
    ):
        """
        生成 PNG 图片预览，模拟 SH1106 OLED 显示效果。

        所有字符排列在网格中，每个像素按 scale 倍放大。
        底部附注字符编码标签。

        Args:
            path: 输出 PNG 文件路径
            start: 起始 ASCII 码
            end: 结束 ASCII 码
            scale: 每个逻辑像素的放大倍数（默认 8）
            cols: 每行显示的字符数（默认 16）
            fg: 前景色（亮像素）RGB
            bg: 背景色（暗像素）RGB
            label_height: 标签行高度（像素）
        """
        from PIL import Image, ImageDraw, ImageFont

        char_count = end - start + 1
        rows = (char_count + cols - 1) // cols

        cell_w = self.width * scale
        cell_h = self.height * scale
        gap = scale  # 字符间距

        img_w = cols * (cell_w + gap) - gap + 2 * scale
        img_h = rows * (cell_h + label_height + gap) - gap + 2 * scale

        img = Image.new("RGB", (img_w, img_h), bg)
        draw = ImageDraw.Draw(img)

        # 尝试加载一个小字体用于标签
        try:
            label_font = ImageFont.truetype(
                "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                max(10, scale),
            )
        except Exception:
            label_font = ImageFont.load_default()

        for idx, code in enumerate(range(start, end + 1)):
            row = idx // cols
            col = idx % cols

            x0 = scale + col * (cell_w + gap)
            y0 = scale + row * (cell_h + label_height + gap)

            bitmap = self._to_msb_bitmap(self._get_bitmap(code))

            # 绘制像素点
            for px_col in range(self.width):
                for px_row in range(self.height):
                    bit = (bitmap[px_col] >> (7 - px_row)) & 0x01
                    color = fg if bit else bg
                    x1 = x0 + px_col * scale
                    y1 = y0 + px_row * scale
                    draw.rectangle(
                        [x1, y1, x1 + scale - 1, y1 + scale - 1],
                        fill=color,
                    )

            # 绘制标签
            char_repr = chr(code) if 0x21 <= code <= 0x7E else "SPC" if code == 0x20 else "?"
            label = f"{char_repr}"
            label_y = y0 + cell_h + 1
            draw.text((x0, label_y), label, fill=(120, 120, 120), font=label_font)

        img.save(path)
        return path


def main():
    parser = argparse.ArgumentParser(
        description="SH1106 OLED 字体库生成工具（支持 TTF/OTF 字体）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "示例:\n"
            "  # 内置 5x8 字体\n"
            "  python sh1106_font_generator.py\n"
            "\n"
            "  # 仅生成数字\n"
            "  python sh1106_font_generator.py --start 48 --end 57 --name font_digits\n"
            "\n"
            "  # 从 TTF 字体渲染 5x8\n"
            "  python sh1106_font_generator.py --ttf /path/to/font.ttf --size 8\n"
            "\n"
            "  # 从 TTF 渲染更大尺寸\n"
            "  python sh1106_font_generator.py --ttf font.ttf --width 8 --height 8 --size 12\n"
            "\n"
            "  # 预览字符点阵\n"
            "  python sh1106_font_generator.py --preview 65 66 67\n"
            "\n"
            "  # 生成 PNG 预览图片\n"
            "  python sh1106_font_generator.py --preview-png font_preview.png\n"
            "  python sh1106_font_generator.py --ttf font.ttf --size 16 --preview-png preview.png\n"
        ),
    )

    parser.add_argument("--start", type=int, default=32, help="起始 ASCII 码 (默认: 32)")
    parser.add_argument("--end", type=int, default=126, help="结束 ASCII 码 (默认: 126)")
    parser.add_argument("--name", type=str, default="font_5x8", help="数组名称 (默认: font_5x8)")
    parser.add_argument("--output", type=str, default=None, help="输出文件路径 (默认: stdout)")
    parser.add_argument(
        "--stride", type=int, default=0,
        help="数组每行字节数 (默认: width+1, 含末尾空列)",
    )
    parser.add_argument(
        "--format", choices=["c", "python"], default="c",
        help="输出格式 (默认: c)",
    )
    parser.add_argument("--preview", nargs="+", type=int, help="预览指定字符的点阵（文本）")
    parser.add_argument(
        "--preview-png", type=str, default=None, metavar="FILE",
        help="生成 PNG 预览图片（模拟 OLED 效果）",
    )
    parser.add_argument(
        "--scale", type=int, default=8,
        help="PNG 预览像素放大倍数（默认: 8）",
    )
    parser.add_argument(
        "--cols", type=int, default=16,
        help="PNG 预览每行字符数（默认: 16）",
    )

    # TTF 字体相关参数
    ttf_group = parser.add_argument_group("TTF 字体选项")
    ttf_group.add_argument(
        "--ttf", type=str, default=None,
        help="TTF/OTF 字体文件路径（使用此选项从字体文件渲染位图）",
    )
    ttf_group.add_argument(
        "--width", type=int, default=5,
        help="字符宽度（像素，默认: 5）",
    )
    ttf_group.add_argument(
        "--height", type=int, default=8,
        help="字符高度（像素，默认: 8，SH1106 单页最大 8）",
    )
    ttf_group.add_argument(
        "--size", type=int, default=8,
        help="TTF 渲染字号（默认: 8）",
    )

    args = parser.parse_args()

    # 创建生成器
    try:
        gen = SH1106FontGenerator(
            font_path=args.ttf,
            width=args.width,
            height=args.height,
            font_size=args.size,
        )
    except ValueError as e:
        print(f"错误: {e}", file=sys.stderr)
        sys.exit(1)

    # 预览模式
    if args.preview:
        for code in args.preview:
            print(gen.preview_char(code))
            print()
        return

    # PNG 预览模式
    if args.preview_png:
        out_path = gen.preview_png(
            args.preview_png,
            start=args.start,
            end=args.end,
            scale=args.scale,
            cols=args.cols,
        )
        print(f"PNG 预览已生成: {out_path}", file=sys.stderr)
        return

    # 生成字体数组
    if args.start > args.end:
        print(f"错误: 起始码 ({args.start}) 不能大于结束码 ({args.end})", file=sys.stderr)
        sys.exit(1)

    if args.format == "c":
        stride = args.stride if args.stride > 0 else args.width + 1
        output = gen.generate_c_array(args.name, args.start, args.end, stride)
    else:
        output = gen.generate_python_dict(args.name, args.start, args.end)

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(output)
            f.write("\n")
        print(f"字体库已写入: {args.output}", file=sys.stderr)
    else:
        print(output)


if __name__ == "__main__":
    main()
