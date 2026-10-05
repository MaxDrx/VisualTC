#!/usr/bin/env python3
"""Draws the background of the macOS disk image (packaging/macos/dmg-background*.png).

    python3 tools/make_dmg_background.py [FONT_REGULAR FONT_BOLD]

Window 600 x 400 points; VisualTC.app is placed at (150, 185) and the
"Aplicativos" link at (450, 185) by packaging/macos/dmg_settings.py.
Light background on purpose: Finder writes the icon names in dark text.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "packaging", "macos")
REGULAR = sys.argv[1] if len(sys.argv) > 2 else "/usr/share/fonts/opentype/inter/Inter-Regular.otf"
BOLD = sys.argv[2] if len(sys.argv) > 2 else "/usr/share/fonts/opentype/inter/Inter-SemiBold.otf"

W, H = 600, 400
ACCENT = (18, 132, 168)
TEXT = (32, 38, 46)
MUTED = (96, 106, 118)


def draw(scale: int) -> Image.Image:
    s = scale
    img = Image.new("RGB", (W * s, H * s))
    px = img.load()
    top, bottom = (250, 251, 253), (232, 237, 242)
    for y in range(H * s):
        t = y / (H * s - 1)
        row = tuple(round(top[i] + (bottom[i] - top[i]) * t) for i in range(3))
        for x in range(W * s):
            px[x, y] = row
    d = ImageDraw.Draw(img)

    title = ImageFont.truetype(BOLD, 22 * s)
    sub = ImageFont.truetype(REGULAR, 13 * s)
    big = ImageFont.truetype(BOLD, 16 * s)
    small = ImageFont.truetype(REGULAR, 12.5 * s)

    def centered(text, font, y, fill):
        w = d.textlength(text, font=font)
        d.text(((W * s - w) / 2, y * s), text, font=font, fill=fill)

    centered("VisualTC", title, 22, TEXT)
    centered("Visualizador de imagens médicas DICOM", sub, 52, MUTED)

    # Arrow from the app icon to the Applications folder.
    y = 185 * s
    x0, x1 = 238 * s, 352 * s
    d.line([(x0, y), (x1 - 14 * s, y)], fill=ACCENT, width=5 * s)
    d.polygon([(x1, y), (x1 - 20 * s, y - 13 * s), (x1 - 20 * s, y + 13 * s)], fill=ACCENT)

    centered("Arraste o VisualTC para a pasta Aplicativos", big, 300, TEXT)
    centered("Depois, abra o VisualTC pelo Launchpad ou pela pasta Aplicativos.", small, 328, MUTED)
    centered("Funciona em Macs com Apple Silicon (M1, M2, M3, M4…) e com Intel.", small, 350, MUTED)
    return img


draw(1).save(os.path.join(OUT, "dmg-background.png"), optimize=True)
draw(2).save(os.path.join(OUT, "dmg-background@2x.png"), optimize=True)
print("ok")
