#!/usr/bin/env python3
"""Generates the BeamIt icon (assets/icons): PNGs, Windows .ico and macOS .icns (needs iconutil on a Mac).

Run from the repository root:  python3 scripts/make_icons.py   (requires Pillow)
"""
import os, random, shutil, subprocess, sys
from PIL import Image, ImageDraw, ImageChops

S = 4096          # working canvas; coordinates below are on a 1024 grid
K = S / 1024
CS = 12  # cursor scale
OUT = os.path.join(os.path.dirname(__file__), '..', 'assets', 'icons')
CURSOR = [(0, 0), (0, 16), (4, 12), (7, 19), (9.5, 18), (6.5, 11.5), (11.5, 11.5)]


def sc(pts):
    return [(x * K, y * K) for x, y in pts]


def gradient(size, top, bottom):
    img = Image.new('RGBA', (1, size[1]))
    for y in range(size[1]):
        t = y / (size[1] - 1)
        img.putpixel((0, y), tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(4)))
    return img.resize(size)


def cursor_poly(tip, scale):
    return [(tip[0] + x * scale, tip[1] + y * scale) for x, y in CURSOR]


def monitor(box, screen_top, screen_bottom):
    layer = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = box
    d.rounded_rectangle([x0 * K, y0 * K, x1 * K, y1 * K], 26 * K, fill=(236, 242, 250, 255))
    inner = Image.new('L', (S, S), 0)
    ImageDraw.Draw(inner).rounded_rectangle([(x0 + 18) * K, (y0 + 18) * K, (x1 - 18) * K, (y1 - 18) * K], 12 * K, fill=255)
    layer.paste(gradient((S, S), screen_top, screen_bottom), (0, 0), inner)
    cx = (x0 + x1) / 2
    d.rectangle([(cx - 14) * K, y1 * K, (cx + 14) * K, (y1 + 46) * K], fill=(236, 242, 250, 255))
    d.rounded_rectangle([(cx - 90) * K, (y1 + 40) * K, (cx + 90) * K, (y1 + 62) * K], 11 * K, fill=(236, 242, 250, 255))
    return layer


def half_mask(poly, split_x, keep_right):
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).polygon(sc(poly), fill=255)
    cut = Image.new('L', (S, S), 0)
    box = [split_x * K, 0, S, S] if keep_right else [0, 0, split_x * K, S]
    ImageDraw.Draw(cut).rectangle(box, fill=255)
    return ImageChops.multiply(m, cut)


def pixels(poly, x_from, x_to, dissolving_right, rng):
    """Cursor fragments on a coarse grid; density falls off towards the dissolving side."""
    full = Image.new('L', (S, S), 0)
    ImageDraw.Draw(full).polygon(sc(poly), fill=255)
    out = Image.new('L', (S, S), 0)
    d = ImageDraw.Draw(out)
    cell = 9
    for gy in range(0, 1024, cell):
        for gx in range(int(x_from), int(x_to), cell):
            t = (gx - x_from) / (x_to - x_from)
            keep = 1 - t if dissolving_right else t
            if full.getpixel(((gx + cell / 2) * K, (gy + cell / 2) * K)) and rng.random() < keep * 0.95:
                d.rectangle([gx * K, gy * K, (gx + cell - 2) * K, (gy + cell - 2) * K], fill=255)
    return out


def globe(img):
    """Wireframe globe behind the monitors to suggest the network."""
    cx, cy, r = 512, 485, 470
    layer = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    box = lambda rx, ry: [(cx - rx) * K, (cy - ry) * K, (cx + rx) * K, (cy + ry) * K]
    d.ellipse(box(r, r), fill=(40, 90, 170, 90))
    line = (120, 190, 255, 150)
    w = int(7 * K)
    d.ellipse(box(r, r), outline=line, width=int(10 * K))
    for rx in (r * 0.35, r * 0.7):
        d.ellipse(box(rx, r), outline=line, width=w)
    d.line([cx * K, (cy - r) * K, cx * K, (cy + r) * K], fill=line, width=w)
    for dy in (-0.5, 0, 0.5):
        y = cy + dy * r
        half = (r * r - (dy * r) ** 2) ** 0.5
        d.line([(cx - half) * K, y * K, (cx + half) * K, y * K], fill=line, width=w)
    clip = Image.new('L', (S, S), 0)
    ImageDraw.Draw(clip).ellipse(box(r + 6, r + 6), fill=255)
    layer.putalpha(ImageChops.multiply(layer.getchannel('A'), clip))
    img.alpha_composite(layer)


def draw_icon(rounded=True):
    rng = random.Random(7)
    img = gradient((S, S), (28, 46, 96, 255), (10, 16, 40, 255))
    if rounded:
        bg = Image.new('L', (S, S), 0)
        ImageDraw.Draw(bg).rounded_rectangle([0, 0, S - 1, S - 1], 224 * K, fill=255)
        img.putalpha(bg)

    globe(img)

    left = monitor((56, 330, 404, 640), (70, 120, 210, 255), (30, 60, 140, 255))
    img.alpha_composite(left)

    right = monitor((620, 330, 968, 640), (70, 120, 210, 255), (30, 60, 140, 255))
    fade = Image.linear_gradient('L').rotate(90, expand=True).resize((int(150 * K), S))
    mask = Image.new('L', (S, S), 255)
    mask.paste(0, (0, 0, int(620 * K), S))
    mask.paste(fade, (int(620 * K), 0))
    right.putalpha(ImageChops.multiply(right.getchannel('A'), mask))
    img.alpha_composite(right)

    white = Image.new('RGBA', (S, S), (255, 255, 255, 255))
    # Left monitor: left half solid, right half dissolving into pixels.
    poly = cursor_poly((228, 362), CS)
    split = 228 + 5.75 * CS
    img.paste(white, (0, 0), half_mask(poly, split, keep_right=False))
    img.paste(white, (0, 0), ImageChops.multiply(pixels(poly, split, 228 + 11.5 * CS, True, rng), half_mask_cut(split, True)))
    # Right monitor: matching other half, assembling from pixels.
    poly2 = cursor_poly((750, 362), CS)
    split2 = 750 + 5.75 * CS
    img.paste(white, (0, 0), half_mask(poly2, split2, keep_right=True))
    img.paste(white, (0, 0), ImageChops.multiply(pixels(poly2, 750, split2, False, rng), half_mask_cut(split2, False)))

    # Beam arrow between the monitors.
    glow = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ad = ImageDraw.Draw(glow)
    arrow = [(436, 470), (520, 470), (520, 420), (600, 485), (520, 550), (520, 500), (436, 500)]
    ad.polygon(sc(arrow), fill=(64, 224, 255, 255))
    img.alpha_composite(glow)
    return img.resize((1024, 1024), Image.LANCZOS)


def half_mask_cut(split_x, keep_right):
    m = Image.new('L', (S, S), 0)
    box = [split_x * K, 0, S, S] if keep_right else [0, 0, split_x * K, S]
    ImageDraw.Draw(m).rectangle(box, fill=255)
    return m


def main():
    os.makedirs(OUT, exist_ok=True)
    master = draw_icon()
    master.save(os.path.join(OUT, 'beamit-1024.png'))
    sizes = [16, 24, 32, 48, 64, 128, 256, 512]
    for s in sizes:
        master.resize((s, s), Image.LANCZOS).save(os.path.join(OUT, f'beamit-{s}.png'))
    master.save(os.path.join(OUT, 'beamit.ico'), sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])

    if sys.platform == 'darwin' and shutil.which('iconutil'):
        iconset = os.path.join(OUT, 'beamit.iconset')
        os.makedirs(iconset, exist_ok=True)
        for base in (16, 32, 128, 256, 512):
            master.resize((base, base), Image.LANCZOS).save(os.path.join(iconset, f'icon_{base}x{base}.png'))
            master.resize((base * 2, base * 2), Image.LANCZOS).save(os.path.join(iconset, f'icon_{base}x{base}@2x.png'))
        subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', os.path.join(OUT, 'beamit.icns')], check=True)
        shutil.rmtree(iconset)


if __name__ == '__main__':
    main()
