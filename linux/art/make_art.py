#!/usr/bin/env python3
"""EmberOS artwork: flat logo, wordmark, boot-menu assets. Wallpaper is a community photo, not generated."""
import math, os, sys
from PIL import Image, ImageDraw, ImageFont

OUT = sys.argv[1]
FONT_DIR = sys.argv[2]               # directory with Ubuntu-R.ttf / Ubuntu-M.ttf
os.makedirs(OUT, exist_ok=True)

ORANGE = (226, 88, 34, 255)          # single brand colour
WHITE = (255, 255, 255, 255)

def flame(cx, base, w, h, curl=0.0, n=160):
    """Teardrop: round bottom, point at the top, optional bend of the tip."""
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x = math.sin(t) * (math.sin(t / 2) ** 1.5)
        up = (1 + math.cos(t)) / 2       # t=0 -> tip (top)
        x += curl * up ** 2.4
        pts.append((cx + x * w / 2, base - up * h))
    return pts

def logo(size):
    """Flat mark: orange disc, white flame with an orange inner tongue."""
    S = size * 8
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.ellipse([0, 0, S - 1, S - 1], fill=ORANGE)
    d.polygon(flame(S * 0.5, S * 0.80, S * 0.50, S * 0.60, curl=0.10), fill=WHITE)
    d.polygon(flame(S * 0.5, S * 0.80, S * 0.22, S * 0.30, curl=0.04), fill=ORANGE)
    return img.resize((size, size), Image.LANCZOS)

def wordmark(height, colour=(236, 236, 236, 255)):
    f = ImageFont.truetype(os.path.join(FONT_DIR, "Ubuntu-M.ttf"), height)
    w = ImageDraw.Draw(Image.new("RGBA", (1, 1))).textlength("EmberOS", font=f)
    img = Image.new("RGBA", (int(w) + 4, int(height * 1.3)), (0, 0, 0, 0))
    ImageDraw.Draw(img).text((0, 0), "EmberOS", font=f, fill=colour)
    return img

for s in (16, 24, 32, 48, 64, 96, 128, 256):
    logo(s).save(os.path.join(OUT, f"logo-{s}.png"))
wordmark(30).save(os.path.join(OUT, "watermark.png"))
logo(96).save(os.path.join(OUT, "splash-logo.png"))
# Boot menu: selection bar pieces (subtle grey)
for n in ["nw", "n", "ne", "w", "c", "e", "sw", "s", "se"]:
    Image.new("RGBA", (4, 4), (255, 255, 255, 28)).save(os.path.join(OUT, f"select_{n}.png"))
print("art ok")
