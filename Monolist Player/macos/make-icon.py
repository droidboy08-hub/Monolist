#!/usr/bin/env python3
"""Draws Monolist.icns, the macOS app icon, from the bundled typeface.

The sidebar's wordmark reduced to its first letter: an ink M in Archivo
ExtraBold on paper, and the signal-red full stop. The paper is a rounded
square on Apple's icon grid (824 of 1024, inset 100), as every Mac app icon
since Big Sur is; the corners belong to the platform, like the window's.

    python3 macos/make-icon.py            # needs Pillow: pip install pillow

Colours are Theme.qml's bg, text and accent. Rerun it if they change.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
FONT = HERE.parent / "fonts" / "Archivo-ExtraBold.ttf"
OUT = HERE / "Monolist.icns"

PAPER = (0xF3, 0xF2, 0xF2, 255)
INK = (0x20, 0x1E, 0x1D, 255)
SIGNAL = (0xEC, 0x30, 0x13, 255)

CANVAS = 1024
TILE = 824
INSET = (CANVAS - TILE) // 2
RADIUS = 185


def draw(size: int = CANVAS) -> Image.Image:
    # Drawn at 4x and reduced, so the curves and the letter are smooth.
    scale = 4
    s = CANVAS * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    tile = (INSET * scale, INSET * scale, (INSET + TILE) * scale, (INSET + TILE) * scale)

    # The shadow the Dock draws under every other icon.
    shadow = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    offset = 10 * scale
    ImageDraw.Draw(shadow).rounded_rectangle(
        (tile[0], tile[1] + offset, tile[2], tile[3] + offset),
        radius=RADIUS * scale, fill=(0, 0, 0, 80))
    image.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14 * scale)))

    ImageDraw.Draw(image).rounded_rectangle(tile, radius=RADIUS * scale, fill=PAPER)

    # The M, placed by its ink rather than its line box — a glyph's box
    # includes side bearings that are not the same on both sides — and set a
    # little left of centre so that the M and the full stop together are.
    font = ImageFont.truetype(str(FONT), 560 * scale)
    letter = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(letter).text((0, 0), "M", font=font, fill=INK)
    ink = letter.getbbox()
    letter = letter.crop(ink)
    width, height = letter.size
    dot = 118 * scale
    gap = 34 * scale
    x = (s - (width + gap + dot)) // 2
    y = (s - height) // 2
    image.alpha_composite(letter, (x, y))

    # The full stop: square, like every other corner in the design, and
    # sitting on the M's baseline.
    dot_left = x + width + gap
    baseline = y + height
    ImageDraw.Draw(image).rectangle((dot_left, baseline - dot, dot_left + dot, baseline), fill=SIGNAL)

    return image.resize((size, size), Image.LANCZOS)


def main() -> None:
    icon = draw()
    icon.save(OUT, format="ICNS",
              sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
