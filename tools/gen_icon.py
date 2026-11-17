#!/usr/bin/env python3
"""Generate the QSO-Zero launcher/store icons (synthwave sun, grid, antenna).
Writes packaging/appstore/icon-256.png and the launcher sizes 100 and 80 px."""
from PIL import Image, ImageDraw
import math, os

S = 512
out = os.path.join(os.path.dirname(__file__), '..', 'packaging', 'appstore')

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))

img = Image.new('RGBA', (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
# background: rounded square, night-sky gradient
bg = Image.new('RGBA', (S, S))
bd = ImageDraw.Draw(bg)
for y in range(S):
    bd.line([(0, y), (S, y)], fill=lerp((13, 2, 33), (58, 12, 102), y / S) + (255,))
mask = Image.new('L', (S, S), 0)
ImageDraw.Draw(mask).rounded_rectangle([0, 0, S - 1, S - 1], radius=96, fill=255)
img.paste(bg, (0, 0), mask)

hz = int(S * 0.64)                     # horizon
# sun with stripes on its lower half
cx, cy, r = S // 2, hz - 8, 150
for y in range(cy - r, hz):
    dy = y - cy
    half = int(math.sqrt(max(0, r * r - dy * dy)))
    t = (y - (cy - r)) / (hz - (cy - r))
    if t > 0.45:
        band = int((t - 0.45) * 40)
        if (y // 9) % 2 == 1 and band >= 1 and (y % 9) < min(2 + band, 7):
            continue
    d.line([(cx - half, y), (cx + half, y)], fill=lerp((249, 200, 14), (255, 42, 109), t) + (255,))
# horizon line
d.rectangle([24, hz, S - 25, hz + 4], fill=(255, 42, 109, 255))
# perspective grid
for k in range(1, 9):
    y = hz + int((S - hz) * (k / 8) ** 1.8)
    d.line([(24, y), (S - 25, y)], fill=(185, 103, 255, 200), width=3)
for k in range(-7, 8):
    x0 = cx + k * 34
    x1 = cx + k * 150
    d.line([(x0, hz), (x1, S - 1)], fill=(185, 103, 255, 170), width=3)
# antenna mast + radiating arcs (cyan)
ax, top = int(S * 0.70), int(S * 0.27)
d.line([(ax, top), (ax, hz)], fill=(5, 217, 232, 255), width=10)
d.line([(ax - 42, hz), (ax, top + 70)], fill=(5, 217, 232, 255), width=7)
d.line([(ax + 42, hz), (ax, top + 70)], fill=(5, 217, 232, 255), width=7)
d.ellipse([ax - 13, top - 13, ax + 13, top + 13], fill=(5, 217, 232, 255))
for i, rr in enumerate((38, 66, 94)):
    a = 255 - i * 55
    d.arc([ax - rr, top - rr, ax + rr, top + rr], 300, 360, fill=(5, 217, 232, a), width=9)
    d.arc([ax - rr, top - rr, ax + rr, top + rr], 0, 60, fill=(5, 217, 232, a), width=9)
    d.arc([ax - rr, top - rr, ax + rr, top + rr], 120, 240, fill=(5, 217, 232, a), width=9)
img.paste(Image.new('RGBA', (S, S), (0, 0, 0, 0)), (0, 0), Image.eval(mask, lambda v: 255 - v))

img.resize((256, 256), Image.LANCZOS).save(os.path.join(out, 'icon-256.png'))
img.resize((100, 100), Image.LANCZOS).save(os.path.join(out, 'qso-zero.png'))
img.resize((100, 100), Image.LANCZOS).save(os.path.join(out, 'qso-zero_100.png'))
img.resize((80, 80), Image.LANCZOS).save(os.path.join(out, 'qso-zero_80.png'))
print('icons written to', os.path.abspath(out))
