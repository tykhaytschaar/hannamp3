#!/usr/bin/env python3
"""
Album cover placeholder generátor (160×160 JPG).

A jpg-t a sd_find_album_art() fallback-en mutatjuk meg, ha nincs cover.jpg
a track mellett. A téma-tokenekhez (COL_BG / COL_BG_PANEL / COL_BG_PANEL_2 /
COL_ACCENT) igazítva, finom vinyl-lemez stílus — nem zajos, nem vonja el
a figyelmet a track címről.

Futtatás:
    pip3 install Pillow
    python3 main/assets/gen_cover_placeholder.py

Output:
    main/assets/cover_placeholder.jpg
"""

from PIL import Image, ImageDraw
import os

W = H = 160

# UI theme tokenek (ui.c-ből)
BG     = (0x0E, 0x11, 0x16)   # COL_BG — screen háttér (és a lyuk színe)
PANEL  = (0x16, 0x1B, 0x22)   # COL_BG_PANEL — kép háttér
DISC   = (0x1F, 0x26, 0x30)   # COL_BG_PANEL_2 — a lemez
GROOVE = (0x2A, 0x33, 0x3F)   # picit halványabb — barázdák
ACCENT = (0x2E, 0xE6, 0xD6)   # COL_ACCENT — center pötty

img = Image.new('RGB', (W, H), PANEL)
draw = ImageDraw.Draw(img, 'RGBA')

cx, cy = W // 2, H // 2

# Nagy lemez — szinte teljes szélesség (r=72 → 144 px átmérő)
r = 72
draw.ellipse([cx - r, cy - r, cx + r, cy + r], fill=DISC)

# Koncentrikus barázdák — finom struktúra
for gr in (64, 56, 48, 40, 32):
    draw.ellipse([cx - gr, cy - gr, cx + gr, cy + gr], outline=GROOVE, width=1)

# Lemezcímke (kisebb belső kör) — picit kontrasztosabb panel-bg-vel
lbl_r = 26
draw.ellipse([cx - lbl_r, cy - lbl_r, cx + lbl_r, cy + lbl_r], fill=PANEL)

# Lyuk a tengelynek
hole_r = 8
draw.ellipse([cx - hole_r, cy - hole_r, cx + hole_r, cy + hole_r], fill=BG)

# Accent center pötty — a téma kiemelőszínével
acc_r = 3
draw.ellipse([cx - acc_r, cy - acc_r, cx + acc_r, cy + acc_r], fill=ACCENT)

# Subtle felső "fényrebbenés" — egy elnyúlt halvány-fehér ellipszis a lemez
# tetején (kvázi-spekuláris highlight)
highlight = Image.new('RGBA', (W, H), (0, 0, 0, 0))
hdraw = ImageDraw.Draw(highlight)
hdraw.ellipse([cx - 58, cy - 66, cx + 58, cy - 30],
              fill=(0xE6, 0xED, 0xF3, 14))
img.paste(highlight, (0, 0), highlight)

out_path = os.path.join(os.path.dirname(__file__), 'cover_placeholder.jpg')
img.save(out_path, 'JPEG', quality=92, optimize=True)
print(f"saved: {out_path}  ({os.path.getsize(out_path)} bytes)")
