#!/usr/bin/env python3
"""Render a folder of procedural, non-personal "photos" for the README
screenshots (driven by make-screenshots.sh). Needs Pillow + numpy.

Nothing here comes from a camera or a third party: every scene is drawn
from gradients, sine-sum ridge lines and one Mandelbrot zoom, with a fixed
RNG seed so a regeneration reproduces the same folder.

Usage: make-images.py OUTDIR
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

W, H = 1600, 1067
RNG = np.random.default_rng(42)


def lerp(a, b, t):
    return a + (b - a) * t


def sky(top, bottom, horizon=1.0):
    """Vertical gradient from top to bottom colour, reaching bottom at
    fraction `horizon` of the height."""
    t = np.clip(np.linspace(0, 1, H)[:, None, None] / horizon, 0, 1)
    img = lerp(np.array(top, float), np.array(bottom, float), t)
    return np.broadcast_to(img, (H, W, 3)).copy()


def ridge(base, rough, octaves=6):
    """1-D fractal ridge line (sum of sines with halving amplitude), as a
    per-column row index."""
    x = np.linspace(0, 1, W)
    y = np.full(W, base, float)
    amp = rough
    for o in range(octaves):
        f = 2 ** o * RNG.uniform(1.5, 3.0)
        y += amp * np.sin(2 * np.pi * (f * x + RNG.uniform()))
        amp *= 0.5
    return (y * H).astype(int)


def paint_layer(img, line, colour, haze=0.0, sky_col=None):
    """Fill everything below `line`; `haze` blends toward the sky colour so
    distant layers fade (aerial perspective). The fill darkens downward
    from the layer's highest peak (one horizontal gradient per layer, so no
    vertical streaks under the peaks): besides looking less flat, it spreads
    each layer over many tones, so the info card's histogram shows smooth
    hills rather than one spike per flat fill."""
    rows = np.arange(H)[:, None]
    mask = rows >= line[None, :]
    col = np.array(colour, float)
    if sky_col is not None:
        col = lerp(col, np.array(sky_col, float), haze)
    shade = 1.0 - 0.45 * np.clip((rows - line.min()) / (0.35 * H), 0, 1)
    img[mask] = (col * shade[..., None] * np.ones((1, W, 1)))[mask]
    return img


def mountains(top, bottom, layers, sun=None):
    img = sky(top, bottom)
    if sun is not None:
        cx, cy, r, col = sun
        yy, xx = np.mgrid[0:H, 0:W]
        d = np.hypot(xx - cx * W, yy - cy * H)
        glow = np.exp(-(d / (r * 4)) ** 2)[..., None]
        img = lerp(img, np.array(col, float), glow * 0.6)
        img[d < r] = col
    for base, rough, col, haze in layers:
        img = paint_layer(img, ridge(base, rough), col, haze, bottom)
    return img


def grid01():
    """Normalised (row, column) coordinates in [0, 1]."""
    yy, xx = np.mgrid[0:H, 0:W]
    return yy / H, xx / W


def dunes():
    img = sky((40, 90, 170), (250, 200, 150), 0.55)
    yy, xx = grid01()
    for i, (base, col) in enumerate([(0.55, (214, 150, 90)),
                                     (0.68, (196, 120, 70)),
                                     (0.82, (170, 95, 55))]):
        phase = xx * (1.3 + i) + i * 0.7
        line = base + 0.05 * np.sin(2 * np.pi * phase)
        shade = 0.85 + 0.15 * np.cos(2 * np.pi * (phase + 0.3))
        m = yy >= line
        img[m] = (np.array(col, float) * shade[..., None])[m]
    return img


def aurora():
    img = sky((5, 10, 30), (15, 40, 60))
    yy, xx = grid01()
    for k, col in enumerate([(60, 255, 150), (120, 80, 255)]):
        c = 0.35 + 0.1 * k + 0.08 * np.sin(2 * np.pi * (xx * (1.5 + k) + k))
        # Curtain: sharp lower edge, long fade upward, fine vertical rays.
        d = yy - c
        band = np.where(d > 0, np.exp(-(d / 0.03) ** 2),
                        np.exp(-(d / 0.18) ** 2))
        streak = 0.7 + 0.3 * np.sin(xx * 700 + 3 * np.sin(xx * 23 + k))
        img += (band * streak)[..., None] * np.array(col, float) * 0.8
    img[RNG.random((H, W)) > 0.9985] = 255
    return paint_layer(img, ridge(0.8, 0.03), (5, 8, 12))


def ocean():
    return ocean_at((255, 150, 90), (255, 220, 170),
                    (230, 140, 110), (20, 60, 110), 90)


def ocean_at(top, horizon, near, deep, sparkle):
    img = sky(top, horizon, 0.5)
    yy, xx = grid01()
    sea = yy > 0.5
    depth = np.clip((yy - 0.5) / 0.5, 0, 1)
    wave = 0.5 + 0.5 * np.sin(xx * 80 + yy * 400)
    water = lerp(np.array(near, float), np.array(deep, float),
                 depth[..., None])
    glitter = wave * np.exp(-((xx - 0.5) / 0.08) ** 2) * (1 - depth)
    water += glitter[..., None] * sparkle
    img[sea] = water[sea]
    return img


def mandelbrot(re0=-0.7454, im0=0.1300, span=0.0014, phase=0.0):
    re = np.linspace(re0, re0 + span, W)
    im = np.linspace(im0, im0 + span * H / W, H)
    c = re[None, :] + 1j * im[:, None]
    z = np.zeros_like(c)
    n = np.zeros(c.shape)
    for _ in range(300):
        m = np.abs(z) <= 4
        z[m] = z[m] ** 2 + c[m]
        n[m] += 1
    t = (n / n.max()) ** 0.5
    return np.stack([255 * (0.5 + 0.5 * np.sin(6.0 * t + p + phase))
                     for p in (0.0, 2.0, 4.0)], -1)


def forest():
    img = mountains((150, 200, 230), (230, 240, 230), [
        (0.45, 0.04, (110, 150, 140), 0.5),
        (0.60, 0.03, (60, 110, 80), 0.2),
    ])
    # Two rows of triangular "pines" in the foreground.
    for row, col in [(0.72, (35, 80, 50)), (0.85, (20, 55, 35))]:
        base = int(H * row)
        jitter = RNG.integers(-8, 8, W // 28 + 1)
        # Clamp: a negative x would turn the slice below into a
        # wrap-around one that paints almost the whole row.
        for x in np.clip(np.arange(0, W, 28) + jitter, 0, W - 1):
            h = int(H * RNG.uniform(0.12, 0.2))
            for dy in range(h):
                half = int(dy * 0.28)
                img[base - h + dy, max(0, x - half):x + half] = col
        img[base:] = col
    return img


SCENES = {
    "alpine-dusk": lambda: mountains((40, 30, 90), (250, 150, 110), [
        (0.50, 0.05, (120, 80, 130), 0.4),
        (0.62, 0.04, (80, 50, 100), 0.2),
        (0.78, 0.03, (35, 25, 50), 0.0)],
        sun=(0.7, 0.48, 60, (255, 220, 170))),
    "blue-ridge": lambda: mountains((120, 170, 230), (225, 235, 245), [
        (0.45, 0.04, (150, 175, 210), 0.3),
        (0.58, 0.04, (100, 130, 180), 0.2),
        (0.72, 0.03, (60, 90, 140), 0.1),
        (0.86, 0.02, (30, 50, 90), 0.0)]),
    "dunes": dunes,
    "aurora": aurora,
    "sunset-sea": ocean,
    "spiral": mandelbrot,
    "pine-valley": forest,
    "red-canyon": lambda: mountains((70, 140, 210), (240, 200, 160), [
        (0.40, 0.06, (200, 110, 70), 0.1),
        (0.60, 0.05, (160, 70, 40), 0.0),
        (0.80, 0.03, (110, 45, 30), 0.0)],
        sun=(0.2, 0.2, 40, (255, 250, 230))),
    "misty-morning": lambda: mountains((200, 210, 220), (250, 240, 225), [
        (0.50, 0.03, (170, 180, 190), 0.5),
        (0.60, 0.03, (130, 145, 160), 0.3),
        (0.72, 0.02, (90, 105, 120), 0.1),
        (0.85, 0.02, (50, 60, 75), 0.0)],
        sun=(0.3, 0.35, 45, (255, 245, 220))),
    "seahorse-valley": lambda: mandelbrot(-0.7470, 0.1080, 0.0060, 2.5),
    "emerald-peaks": lambda: mountains((30, 110, 120), (200, 235, 210), [
        (0.42, 0.07, (90, 160, 140), 0.3),
        (0.58, 0.05, (40, 120, 100), 0.1),
        (0.76, 0.03, (15, 70, 60), 0.0)]),
    "violet-hour": lambda: mountains((20, 10, 50), (200, 90, 150), [
        (0.55, 0.04, (110, 50, 120), 0.3),
        (0.68, 0.03, (60, 25, 80), 0.1),
        (0.82, 0.02, (25, 10, 40), 0.0)],
        sun=(0.5, 0.56, 70, (255, 200, 200))),
    "golden-hills": lambda: mountains((90, 150, 220), (255, 225, 160), [
        (0.55, 0.02, (220, 180, 90), 0.2),
        (0.66, 0.02, (200, 150, 60), 0.1),
        (0.78, 0.015, (160, 110, 40), 0.0),
        (0.90, 0.01, (110, 75, 30), 0.0)],
        sun=(0.8, 0.25, 50, (255, 250, 220))),
    "night-sea": lambda: ocean_at((10, 20, 60), (60, 70, 140),
                                  (70, 80, 150), (5, 10, 30), 60),
    "ice-field": lambda: mountains((150, 200, 255), (240, 250, 255), [
        (0.48, 0.05, (220, 235, 250), 0.2),
        (0.62, 0.04, (180, 210, 240), 0.1),
        (0.80, 0.01, (235, 245, 255), 0.0)]),
}


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for i, (name, fn) in enumerate(SCENES.items(), 1):
        # Soften the hard layer edges. No grain: noise would make the
        # committed screenshots PNGs several times larger.
        arr = np.clip(fn(), 0, 255).astype(np.uint8)
        im = Image.fromarray(arr).filter(ImageFilter.GaussianBlur(0.8))
        # A little neutral EXIF so the info card has something to show;
        # it names the generator, not a camera.
        exif = Image.Exif()
        exif[0x010F] = "ggaze"                      # Make
        exif[0x0110] = "make-images.py"             # Model
        exif[0x0131] = "procedural render"          # Software
        exif[0x0132] = "2026:09:24 10:%02d:00" % i  # DateTime
        im.save(out / ("%02d-%s.jpg" % (i, name)), quality=90, exif=exif)


if __name__ == "__main__":
    main()
