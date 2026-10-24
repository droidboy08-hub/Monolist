#!/usr/bin/env python3
"""Draws the Mac's app icons from the owner's artwork.

The icon is the owner's (2026-10-02): a black note on a soft white rounded
square, packaging/icons/note.png, with the red note beside it
(packaging/make-icon.ps1 draws that one, and both sets of Windows PNGs).
Their rounded square fills the picture, as Windows' icons do. A Mac's sits on
Apple's icon grid (824 of 1024, inset 100), with the shadow the Dock draws
under every other icon; this puts them there.

It also draws the first icon, kept as a choice (the owner's, 2026-10-24):
the sidebar's wordmark reduced to its first letter, an ink M in Archivo
ExtraBold and the signal-red full stop, now on the note's own white square,
so the three are one family.

Writes:
    macos/Monolist.icns         the bundle's icon, the note
    icons/app/m-<size>.png      the M at Windows' sizes (Settings' preview)
    icons/app/mac-<id>.png      each choice on Apple's grid, 512 px, for the
                                Dock while Monolist runs (AppIcon)

    python3 macos/make-icon.py            # needs Pillow: pip install pillow

Colours are Theme.qml's text and accent. Rerun it if they, or the artwork,
change, and commit what it writes.
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
FONT = ROOT / "fonts" / "Archivo-ExtraBold.ttf"
ARTWORK = ROOT / "packaging" / "icons"
APP_ICONS = ROOT / "icons" / "app"

INK = (0x20, 0x1E, 0x1D, 255)
SIGNAL = (0xEC, 0x30, 0x13, 255)

CANVAS = 1024
TILE = 824
INSET = (CANVAS - TILE) // 2


def blank_square() -> Image.Image:
    """The note's white square with the note taken off it. Inside the note's
    reach (where any pixel is darker than the white around it, and as far
    again as its soft shadow spreads) each row takes its own ground, read off
    a column clear of the note; the square's edges and their shading, far
    from it, are left as they are."""
    note = Image.open(ARTWORK / "note.png").convert("RGBA")
    pixels = note.load()
    clear_x = CANVAS * 150 // 1024
    darker = [(x, y) for y in range(0, CANVAS, 2) for x in range(clear_x, CANVAS - clear_x, 2)
              if pixels[x, y][3] == 255 and sum(pixels[x, y][:3]) < 3 * 200]
    shadow = 48
    left = max(min(x for x, _ in darker) - shadow, clear_x + 1)
    right = min(max(x for x, _ in darker) + shadow, CANVAS - clear_x - 1)
    top = min(y for _, y in darker) - shadow
    bottom = max(y for _, y in darker) + shadow
    for y in range(top, bottom + 1):
        ground = pixels[clear_x, y]
        for x in range(left, right + 1):
            pixels[x, y] = ground[:3] + (pixels[x, y][3],)
    return note


def draw_m() -> Image.Image:
    """The M. on the white square, filling the picture as the notes do."""
    scale = 4
    s = CANVAS * scale
    square = blank_square().resize((s, s), Image.LANCZOS)

    # The first icon's proportions (a 560 px M on an 824 px square), placed
    # by its ink rather than its line box, a little left of centre so that
    # the M and the full stop together are.
    k = CANVAS / TILE
    font = ImageFont.truetype(str(FONT), round(560 * k * scale))
    letter = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(letter).text((0, 0), "M", font=font, fill=INK)
    letter = letter.crop(letter.getbbox())
    width, height = letter.size
    dot = round(118 * k * scale)
    gap = round(34 * k * scale)
    x = (s - (width + gap + dot)) // 2
    y = (s - height) // 2
    square.alpha_composite(letter, (x, y))
    # Square, like every other corner in the design, on the M's baseline.
    left = x + width + gap
    baseline = y + height
    ImageDraw.Draw(square).rectangle((left, baseline - dot, left + dot, baseline), fill=SIGNAL)
    return square.resize((CANVAS, CANVAS), Image.LANCZOS)


def on_grid(icon: Image.Image) -> Image.Image:
    """A picture that fills its square, set on Apple's grid with the Dock's
    shadow under it."""
    tile = icon.resize((TILE, TILE), Image.LANCZOS)
    out = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    shadow = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    mask = tile.getchannel("A").point(lambda v: v * 80 // 255)
    shadow.paste((0, 0, 0, 255), (INSET, INSET + 10), mask)
    out.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(14)))
    out.alpha_composite(tile, (INSET, INSET))
    return out


def main() -> None:
    m = draw_m()
    for size in (16, 24, 32, 48, 64, 128, 256):
        m.resize((size, size), Image.LANCZOS).save(APP_ICONS / f"m-{size}.png", optimize=True)

    choices = {
        "note": Image.open(ARTWORK / "note.png").convert("RGBA"),
        "note-red": Image.open(ARTWORK / "note-red.png").convert("RGBA"),
        "m": m,
    }
    for name, icon in choices.items():
        grid = on_grid(icon)
        grid.resize((512, 512), Image.LANCZOS).save(APP_ICONS / f"mac-{name}.png", optimize=True)
        if name == "note":
            grid.save(HERE / "Monolist.icns", format="ICNS",
                      sizes=[(16, 16), (32, 32), (64, 64), (128, 128), (256, 256), (512, 512), (1024, 1024)])
    print("wrote", HERE / "Monolist.icns", "and", APP_ICONS / "m-*.png, mac-*.png")


if __name__ == "__main__":
    main()
