"""Generates the Web API Testing Toolkit (watt) application icon.

    python resources/icon/make_icon.py

Writes, next to this script:
    watt.ico       Windows icon: 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 px
    watt-256.png   for documentation
    watt-1024.png  master image

Design: a lightning bolt (watt, power) between code chevrons (API), on a dark rounded square
with a blue gradient. Sizes up to 32 px use a bolt-only drawing with a thicker outline, so the
icon stays legible in the taskbar, title bar and Explorer's small views. Everything is drawn
from vector shapes (no fonts). Needs Pillow (pip install pillow).
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

HERE = Path(__file__).resolve().parent

BG_TOP = (30, 58, 138)      # blue-900
BG_BOTTOM = (8, 15, 36)     # near black navy
BOLT_TOP = (253, 224, 71)   # yellow-300
BOLT_BOTTOM = (245, 158, 11)  # amber-500
CHEVRON = (147, 197, 253)   # blue-300

# Bolt outline in unit coordinates (0..1), clockwise from the top left.
BOLT_FULL = [(0.555, 0.17), (0.355, 0.535), (0.485, 0.535), (0.425, 0.83),
             (0.655, 0.445), (0.52, 0.445), (0.625, 0.17)]
BOLT_SMALL = [(0.54, 0.08), (0.24, 0.56), (0.45, 0.56), (0.36, 0.93),
              (0.78, 0.42), (0.55, 0.42), (0.70, 0.08)]


def vgradient(size, top, bottom):
    g = Image.new("RGBA", (1, size))
    for y in range(size):
        t = y / max(1, size - 1)
        g.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,))
    return g.resize((size, size))


def rounded_mask(size, radius):
    m = Image.new("L", (size, size), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return m


def poly(points, size, inset=0.0):
    return [(inset * size + x * size * (1 - 2 * inset), inset * size + y * size * (1 - 2 * inset)) for x, y in points]


def render(size, small):
    """One icon image drawn at 8x and scaled down (smooth edges at every size)."""
    s = size * 8
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # Background tile: rounded square with a vertical gradient and a faint top highlight.
    margin = 0 if small else int(s * 0.03)
    tile = s - 2 * margin
    radius = int(tile * (0.20 if small else 0.22))
    bg = vgradient(tile, BG_TOP, BG_BOTTOM)
    hl = Image.new("RGBA", (tile, tile), (0, 0, 0, 0))
    ImageDraw.Draw(hl).ellipse((-tile * 0.3, -tile * 0.85, tile * 1.3, tile * 0.45), fill=(255, 255, 255, 22))
    bg = Image.alpha_composite(bg, hl.filter(ImageFilter.GaussianBlur(tile * 0.06)))
    img.paste(bg, (margin, margin), rounded_mask(tile, radius))

    # Bolt: amber gradient, with a soft glow under it at the larger sizes.
    pts = poly(BOLT_SMALL if small else BOLT_FULL, tile)
    pts = [(x + margin, y + margin) for x, y in pts]
    bolt_mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(bolt_mask).polygon(pts, fill=255)
    if not small:
        glow = Image.new("RGBA", (s, s), BOLT_BOTTOM + (0,))
        glow.putalpha(bolt_mask.filter(ImageFilter.GaussianBlur(s * 0.035)).point(lambda v: int(v * 0.55)))
        img = Image.alpha_composite(img, glow)
    bolt = vgradient(s, BOLT_TOP, BOLT_BOTTOM)
    img.paste(bolt, (0, 0), bolt_mask)

    # Code chevrons < > beside the bolt (left out at small sizes, where they would blur).
    if not small:
        d = ImageDraw.Draw(img)
        w = int(tile * 0.065)
        cy = margin + tile * 0.5
        h = tile * 0.17
        lx, rx = margin + tile * 0.30, margin + tile * 0.70
        dx = tile * 0.115
        for line in ([(lx, cy - h), (lx - dx, cy), (lx, cy + h)], [(rx, cy - h), (rx + dx, cy), (rx, cy + h)]):
            d.line(line, fill=CHEVRON + (255,), width=w, joint="curve")
            for x, y in (line[0], line[2]):
                d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=CHEVRON + (255,))
    return img.resize((size, size), Image.LANCZOS)


def main():
    master = render(1024, small=False)
    master.save(HERE / "watt-1024.png")
    master.resize((256, 256), Image.LANCZOS).save(HERE / "watt-256.png")
    sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
    frames = [render(n, small=n <= 32) for n in sizes]
    # Pillow writes each frame given in append_images at its own size; 256 is stored as PNG.
    frames[-1].save(HERE / "watt.ico", format="ICO", sizes=[(n, n) for n in sizes], append_images=frames[:-1])
    print("wrote", ", ".join(p.name for p in sorted(HERE.glob("watt*"))))


if __name__ == "__main__":
    main()
