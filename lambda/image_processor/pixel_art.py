"""Small, dependency-light pixel-art transform built specifically for the photo frame.

This is an original Pillow implementation. It does not import or reuse code from
third-party image-to-pixel projects.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Literal

from PIL import Image, ImageOps

DitherMode = Literal["none", "floyd-steinberg"]


@dataclass(frozen=True, slots=True)
class PixelArtOptions:
    """Controls the frame's optional pixel-art appearance."""

    block_size: int = 8
    palette_size: int = 32
    dither: DitherMode = "none"
    background: tuple[int, int, int] = (0, 0, 0)

    def validate(self) -> None:
        if not 1 <= self.block_size <= 128:
            raise ValueError("block_size must be between 1 and 128")
        if not 2 <= self.palette_size <= 256:
            raise ValueError("palette_size must be between 2 and 256")
        if self.dither not in ("none", "floyd-steinberg"):
            raise ValueError("dither must be 'none' or 'floyd-steinberg'")
        if len(self.background) != 3 or any(not 0 <= channel <= 255 for channel in self.background):
            raise ValueError("background must be an RGB tuple with values from 0 to 255")


def _flatten_transparency(image: Image.Image, background: tuple[int, int, int]) -> Image.Image:
    """Composite transparent inputs explicitly because RGB565 has no alpha channel."""
    if image.mode in ("RGBA", "LA") or "transparency" in image.info:
        foreground = image.convert("RGBA")
        base = Image.new("RGBA", foreground.size, (*background, 255))
        return Image.alpha_composite(base, foreground).convert("RGB")
    return image.convert("RGB")


def pixelate(
    image: Image.Image,
    output_size: tuple[int, int] = (800, 480),
    options: PixelArtOptions | None = None,
) -> Image.Image:
    """Return a fitted, palette-reduced pixel-art image at ``output_size``.

    The source is first fitted to the display aspect ratio. It is then reduced to
    a logical canvas based on ``block_size``, quantized, and enlarged with
    nearest-neighbor sampling so every logical pixel remains crisp.
    """
    options = options or PixelArtOptions()
    options.validate()

    width, height = output_size
    if width <= 0 or height <= 0:
        raise ValueError("output_size dimensions must be positive")

    rgb = _flatten_transparency(image, options.background)
    fitted = ImageOps.fit(rgb, output_size, method=Image.Resampling.LANCZOS)

    logical_size = (
        max(1, round(width / options.block_size)),
        max(1, round(height / options.block_size)),
    )
    logical = fitted.resize(logical_size, Image.Resampling.BOX)

    dither = (
        Image.Dither.FLOYDSTEINBERG
        if options.dither == "floyd-steinberg"
        else Image.Dither.NONE
    )
    reduced = logical.quantize(
        colors=options.palette_size,
        method=Image.Quantize.MEDIANCUT,
        dither=dither,
    ).convert("RGB")

    return reduced.resize(output_size, Image.Resampling.NEAREST)
