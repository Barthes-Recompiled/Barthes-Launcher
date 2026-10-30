"""Draws res/app.ico: three cloth-bound spines on the shelf's dark wall (the launcher's own picture, no game art)."""
import colorsys
import sys
from pathlib import Path

from PIL import Image, ImageDraw

def binding(h, v=0.50, s=0.55):
    r, g, b = colorsys.hsv_to_rgb(h, s, v)
    return int(r * 255), int(g * 255), int(b * 255), 255

def draw(size):
    k = size / 256
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=int(40 * k), fill=(22, 24, 30, 255))
    floor = 214 * k
    books = [(0.00, 44, 150), (0.38, 100, 166), (0.62, 156, 140)]
    for hue, x, h in books:
        x0, x1 = x * k, (x + 50) * k
        top = floor - h * k
        d.rounded_rectangle([x0, top, x1, floor], radius=max(1, int(4 * k)), fill=binding(hue))
        band = binding(hue, 0.30, 0.45)
        for y in (top + 14 * k, floor - 22 * k):
            d.rectangle([x0, y, x1, y + 5 * k], fill=band)
    d.rectangle([24 * k, floor, 232 * k, floor + 7 * k], fill=(240, 233, 218, 255))
    return im

out = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "res" / "app.ico")
big = draw(256)
big.save(out, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
print("wrote", out)
