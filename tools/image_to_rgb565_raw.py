#!/usr/bin/env python3
"""Resize a photo smoothly to 800x480 and emit native little-endian RGB565."""

from __future__ import annotations

import argparse
import struct
from pathlib import Path

from PIL import Image, ImageOps

LCD_SIZE = (800, 480)


def rgb565(red: int, green: int, blue: int) -> int:
    return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)


def rgb565_to_rgb888(value: int) -> tuple[int, int, int]:
    red = (value >> 11) & 0x1F
    green = (value >> 5) & 0x3F
    blue = value & 0x1F
    return (
        (red * 255 + 15) // 31,
        (green * 255 + 31) // 63,
        (blue * 255 + 15) // 31,
    )


def convert(source: Path, output: Path, preview: Path | None) -> None:
    with Image.open(source) as opened:
        image = ImageOps.exif_transpose(opened).convert("RGB")

    fitted = ImageOps.fit(image, LCD_SIZE, method=Image.Resampling.LANCZOS)
    values = [rgb565(red, green, blue) for red, green, blue in fitted.get_flattened_data()]

    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as raw:
        for offset in range(0, len(values), 4096):
            chunk = values[offset : offset + 4096]
            raw.write(struct.pack(f"<{len(chunk)}H", *chunk))

    if preview is not None:
        preview.parent.mkdir(parents=True, exist_ok=True)
        display_pixels = [rgb565_to_rgb888(value) for value in values]
        display_preview = Image.new("RGB", LCD_SIZE)
        display_preview.putdata(display_pixels)
        display_preview.save(preview)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="source JPEG or PNG")
    parser.add_argument("output", type=Path, help="output 800x480 RGB565 binary")
    parser.add_argument("--preview", type=Path, help="optional PNG preview of the RGB565 output")
    return parser.parse_args()


if __name__ == "__main__":
    arguments = parse_args()
    convert(arguments.source, arguments.output, arguments.preview)
