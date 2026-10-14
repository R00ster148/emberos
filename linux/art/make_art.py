#!/usr/bin/env python3
"""Draws EmberOS artwork: flame logo, wallpaper, boot splash and boot-menu background."""
import math, os, random, sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

OUT = sys.argv[1]
FONT_DIR = sys.argv[2]               # directory with Ubuntu-B.ttf / Ubuntu-L.ttf
os.makedirs(OUT, exist_ok=True)
random.seed(7)

def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)

def flame_points(cx, base_y, w, h, curl, n=120):
    """Teardrop flame: round bottom, pointed top with a slight curl."""
    pts = []
    for i in range(n):
        t = 2 * math.pi * i / n
        x = math.sin(t) * (math.sin(t / 2) ** 1.6)
        y = math.cos(t)                        # 1 at the round end .. -1 at the point
        up = (1 + y) / 2                       # t=0 (narrow end) at the top, round end at the bottom
        x = x + curl * up ** 2.2               # bend the tip
        pts.append((cx + x * w / 2, base_y - up * h))
    return pts

def logo(size):
    """Layered flame on transparent background, with a soft glow."""
    S = size * 4                               # supersample
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow)
    cx, base = S * 0.5, S * 0.90
    layers = [  # (width, height, curl, colour)
        (0.66, 0.80, 0.16, (205, 45, 20, 255)),
        (0.52, 0.62, 0.20, (240, 95, 20, 255)),
        (0.37, 0.44, 0.16, (255, 160, 30, 255)),
        (0.21, 0.25, 0.08, (255, 228, 120, 255)),
    ]
    gd.polygon(flame_points(cx, base, S * 0.74, S * 0.86, 0.16), fill=(255, 80, 20, 140))
    glow = glow.filter(ImageFilter.GaussianBlur(S * 0.05))
    img = Image.alpha_composite(img, glow)
    d = ImageDraw.Draw(img)
    for w, h, c, col in layers:
        d.polygon(flame_points(cx, base, S * w, S * h, c), fill=col)
    return img.resize((size, size), Image.LANCZOS)

def wordmark(height, colour=(255, 236, 220, 255)):
    f = font("Ubuntu-B.ttf", height)
    g = font("Ubuntu-L.ttf", height)
    tmp = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    w1 = tmp.textlength("Ember", font=f); w2 = tmp.textlength("OS", font=g)
    img = Image.new("RGBA", (int(w1 + w2) + 4, int(height * 1.3)), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.text((0, 0), "Ember", font=f, fill=colour)
    d.text((w1, 0), "OS", font=g, fill=(255, 150, 60, 255))
    return img

def background(W, H, particles=True, big_logo=True, text=True):
    img = Image.new("RGB", (W, H))
    px = img.load()
    cx, cy = W * 0.62, H * 1.05
    maxd = math.hypot(W, H)
    for y in range(H):
        for x in range(W):
            d = math.hypot(x - cx, y - cy) / maxd
            k = max(0.0, 1 - d * 1.55)
            r = int(14 + 110 * k ** 2.2); g = int(12 + 34 * k ** 3); b = int(18 + 8 * k)
            px[x, y] = (r, g, b)
    img = img.convert("RGBA")
    if particles:
        layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(layer)
        for _ in range(int(W * H / 9000)):
            x = random.gauss(W * 0.62, W * 0.22); y = H - abs(random.gauss(0, H * 0.45))
            r = random.choice([1, 1, 1.5, 2, 2.5, 3.5])
            a = int(random.uniform(60, 230) * (y / H))
            col = random.choice([(255, 140, 40), (255, 190, 80), (255, 90, 30), (255, 220, 140)])
            d.ellipse([x - r, y - r, x + r, y + r], fill=col + (a,))
        glow = layer.filter(ImageFilter.GaussianBlur(4))
        img = Image.alpha_composite(img, glow)
        img = Image.alpha_composite(img, layer)
    if big_logo:
        L = logo(int(H * 0.42))
        a = L.split()[3].point(lambda v: v * 0.16)
        L.putalpha(a)
        img.alpha_composite(L, (int(W * 0.62 - L.width / 2), int(H * 0.30)))
    if text:
        wm = wordmark(int(H * 0.028), (255, 236, 220, 170))
        img.alpha_composite(wm, (W - wm.width - int(W * 0.025), H - wm.height - int(H * 0.075)))
    return img.convert("RGB")

# Logo icons
for s in (16, 24, 32, 48, 64, 128, 256):
    logo(s).save(os.path.join(OUT, f"logo-{s}.png"))
# Wallpaper (full HD) and lighter 4:3 version for the boot menu
background(1920, 1080).save(os.path.join(OUT, "wallpaper.png"), optimize=True)
grub = background(1024, 768, particles=True, big_logo=False, text=False)
gi = grub.convert("RGBA")
L = logo(150); gi.alpha_composite(L, (int(512 - 75), 70))
wm = wordmark(54); gi.alpha_composite(wm, (int(512 - wm.width / 2), 230))
gi.convert("RGB").save(os.path.join(OUT, "grub-background.png"))
# Boot splash pieces
wordmark(46).save(os.path.join(OUT, "watermark.png"))
logo(160).save(os.path.join(OUT, "splash-logo.png"))
print("art ok")
