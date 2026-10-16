#!/usr/bin/env python3
"""Draws the Mala Idea steampunk wallpaper for the XDJ-AZ screen (1280x800).

    python3 -m pip install pillow numpy
    python3 tools/make_wallpaper.py

Writes:
    images/wallpaper.png                          the full-colour preview
    mala-idea-wallpaper/assets/wallpaper.miw      what the mod loads on the deck

wallpaper.miw is deliberately simple so the mod needs no image library:
    "MIW1"  u16 width  u16 height  (little endian)
    256 x RGB palette (768 bytes)
    width*height palette indices, row by row
"""
import math
import os
import random
import struct

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

W, H = 1280, 800
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MONO = "/System/Library/Fonts/Menlo.ttc"  # any monospaced font works

# The logo, in ASCII. Each letter is six rows tall (figlet's "big" style).
LETTERS = {
    "M": [" __  __ ", "|  \\/  |", "| \\  / |", "| |\\/| |", "| |  | |", "|_|  |_|"],
    "A": ["          ", "    /\\    ", "   /  \\   ", "  / /\\ \\  ", " / ____ \\ ", "/_/    \\_\\"],
    "L": [" _      ", "| |     ", "| |     ", "| |     ", "| |____ ", "|______|"],
    "I": [" _____ ", "|_   _|", "  | |  ", "  | |  ", " _| |_ ", "|_____|"],
    "D": [" _____  ", "|  __ \\ ", "| |  | |", "| |  | |", "| |__| |", "|_____/ "],
    "E": [" ______ ", "|  ____|", "| |__   ", "|  __|  ", "| |____ ", "|______|"],
    " ": ["   "] * 6,
}


def ascii_word(word):
    rows = [""] * 6
    for ch in word:
        for i, part in enumerate(LETTERS[ch]):
            rows[i] += part
    return rows


def logo_lines():
    """MALA IDEA in ASCII letters; the bubble around it is the parchment plate."""
    return ascii_word("MALA IDEA")


# ---- drawing helpers ---------------------------------------------------------------------

def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def gear(size, teeth, color, dark, hole_ratio=0.18, spokes=6, seed=0):
    """A brass gear on a transparent square, shaded with a fake light from the top left."""
    s = size * 2  # draw large, scale down for clean edges
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = s / 2
    r_out, r_root = s * 0.48, s * 0.41
    pts = []
    for i in range(teeth * 4):
        a = 2 * math.pi * i / (teeth * 4)
        r = r_out if (i % 4) in (1, 2) else r_root
        pts.append((c + r * math.cos(a), c + r * math.sin(a)))
    d.polygon(pts, fill=color)
    # rim and hub
    d.ellipse([c - s * 0.34, c - s * 0.34, c + s * 0.34, c + s * 0.34], fill=dark)
    d.ellipse([c - s * 0.30, c - s * 0.30, c + s * 0.30, c + s * 0.30], fill=(0, 0, 0, 0))
    for k in range(spokes):
        a = 2 * math.pi * k / spokes + seed
        x2, y2 = c + s * 0.31 * math.cos(a), c + s * 0.31 * math.sin(a)
        d.line([(c, c), (x2, y2)], fill=color, width=int(s * 0.055))
    d.ellipse([c - s * 0.12, c - s * 0.12, c + s * 0.12, c + s * 0.12], fill=color)
    d.ellipse([c - s * hole_ratio / 2, c - s * hole_ratio / 2,
               c + s * hole_ratio / 2, c + s * hole_ratio / 2], fill=(0, 0, 0, 0))
    img = img.resize((size, size), Image.LANCZOS)

    # shading: brighten towards the top left, darken towards the bottom right
    a = np.asarray(img).astype(np.float32)
    yy, xx = np.mgrid[0:size, 0:size] / size
    light = 1.25 - 0.6 * (xx * 0.5 + yy * 0.5)
    a[..., :3] = np.clip(a[..., :3] * light[..., None], 0, 255)
    return Image.fromarray(a.astype(np.uint8), "RGBA")


def paste_shadowed(base, im, xy, blur=10, offset=(8, 10), alpha=150):
    sh = Image.new("RGBA", im.size, (0, 0, 0, 0))
    sh.putalpha(im.getchannel("A").point(lambda v: v * alpha // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    base.alpha_composite(sh, (xy[0] + offset[0], xy[1] + offset[1]))
    base.alpha_composite(im, xy)


def rivet(d, x, y, r=5):
    d.ellipse([x - r, y - r, x + r, y + r], fill=(60, 38, 18))
    d.ellipse([x - r + 1, y - r + 1, x + r - 2, y + r - 2], fill=(196, 150, 84))
    d.ellipse([x - r + 2, y - r + 2, x - 1, y - 1], fill=(246, 214, 150))


def pipe_h(base, y, x0, x1, th, joints):
    """A horizontal copper pipe with flanged joints."""
    p = Image.new("RGBA", (x1 - x0, th), (0, 0, 0, 0))
    a = np.zeros((th, x1 - x0, 4), np.float32)
    t = np.linspace(-1, 1, th)
    shade = 0.35 + 0.65 * np.sqrt(np.clip(1 - t ** 2, 0, 1)) + 0.25 * np.exp(-((t + 0.45) ** 2) * 18)
    col = np.array([168, 92, 48], np.float32)
    a[..., :3] = np.clip(col[None, None, :] * shade[:, None, None], 0, 255)
    a[..., 3] = 255
    p = Image.fromarray(a.astype(np.uint8), "RGBA")
    paste_shadowed(base, p, (x0, y), blur=8, offset=(0, 10), alpha=170)
    d = ImageDraw.Draw(base)
    for jx in joints:
        fw, fh = 26, th + 18
        fl = Image.new("RGBA", (fw, fh), (0, 0, 0, 0))
        fa = np.zeros((fh, fw, 4), np.float32)
        tt = np.linspace(-1, 1, fw)
        sh = 0.45 + 0.55 * np.sqrt(np.clip(1 - tt ** 2, 0, 1))
        fa[..., :3] = np.clip(np.array([190, 146, 74])[None, None, :] * sh[None, :, None], 0, 255)
        fa[..., 3] = 255
        fl = Image.fromarray(fa.astype(np.uint8), "RGBA")
        paste_shadowed(base, fl, (jx - fw // 2, y - 9), blur=6, offset=(4, 6), alpha=160)
        rivet(d, jx, y - 2, 4)
        rivet(d, jx, y + th + 2, 4)


def gauge(base, cx, cy, r, needle_deg):
    g = Image.new("RGBA", (r * 2 + 40, r * 2 + 40), (0, 0, 0, 0))
    d = ImageDraw.Draw(g)
    c = r + 20
    d.ellipse([c - r - 12, c - r - 12, c + r + 12, c + r + 12], fill=(150, 104, 46))
    d.ellipse([c - r - 7, c - r - 7, c + r + 7, c + r + 7], fill=(214, 170, 92))
    d.ellipse([c - r, c - r, c + r, c + r], fill=(232, 216, 178))
    for k in range(0, 11):
        a = math.radians(135 + k * 27)
        r1 = r * (0.78 if k % 5 else 0.70)
        d.line([(c + r1 * math.cos(a), c + r1 * math.sin(a)),
                (c + r * 0.9 * math.cos(a), c + r * 0.9 * math.sin(a))],
               fill=(60, 36, 20), width=3 if k % 5 == 0 else 2)
    d.arc([c - r * 0.9, c - r * 0.9, c + r * 0.9, c + r * 0.9], 45 - 54, 45, fill=(150, 30, 20), width=5)
    a = math.radians(needle_deg)
    d.line([(c, c), (c + r * 0.8 * math.cos(a), c + r * 0.8 * math.sin(a))], fill=(120, 20, 12), width=4)
    d.ellipse([c - 7, c - 7, c + 7, c + 7], fill=(60, 36, 20))
    # glass glint
    gl = Image.new("L", g.size, 0)
    ImageDraw.Draw(gl).ellipse([c - r * 0.8, c - r * 0.85, c + r * 0.2, c - r * 0.1], fill=60)
    g.alpha_composite(Image.merge("RGBA", (gl.point(lambda v: 255),) * 3 + (gl.filter(ImageFilter.GaussianBlur(8)),)))
    paste_shadowed(base, g, (cx - c, cy - c), blur=10, offset=(6, 10), alpha=170)


# ---- the scene ---------------------------------------------------------------------------

def background():
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32)
    top, bot = np.array([42, 27, 16], np.float32), np.array([16, 10, 7], np.float32)
    t = (yy / H)[..., None]
    img = top * (1 - t) + bot * t
    # warm glow behind the logo
    gx, gy = W * 0.5, H * 0.42
    glow = np.exp(-(((xx - gx) / 520) ** 2 + ((yy - gy) / 330) ** 2))
    img += glow[..., None] * np.array([70, 40, 14], np.float32)
    # riveted panel seams, like boiler plates
    for x in (320, 960):
        img[:, x - 1:x + 1] *= 0.55
        img[:, x + 1:x + 2] *= 1.25
    for y in (200, 600):
        img[y - 1:y + 1, :] *= 0.55
        img[y + 1:y + 2, :] *= 1.25
    # grain
    rng = np.random.default_rng(7)
    img += rng.normal(0, 4.0, (H, W, 1))
    # vignette
    v = 1 - 0.55 * (((xx - W / 2) / (W / 2)) ** 2 + ((yy - H / 2) / (H / 2)) ** 2) / 2
    img *= np.clip(v, 0.3, 1)[..., None]
    base = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    d = ImageDraw.Draw(base)
    for x in (320, 960):
        for y in range(28, H, 58):
            rivet(d, x + 10, y, 4)
    for y in (200, 600):
        for x in range(30, W, 64):
            if abs(x - 320) > 20 and abs(x - 960) > 20:
                rivet(d, x, y + 10, 4)
    return base


def bubble_plate(lines, font, cw, lh):
    """The logo: a parchment speech bubble (pill body, tail at the bottom left, like the
    original), the ASCII letters inked on it."""
    cols = max(len(l) for l in lines)
    tw, th = cols * cw, len(lines) * lh
    padx, pady, tail = 70, 46, 110
    bw, bh = tw + padx * 2, th + pady * 2
    pw, ph = bw + 20, bh + tail
    k = 2  # supersampling for the mask
    m = Image.new("L", (pw * k, ph * k), 0)
    md = ImageDraw.Draw(m)
    md.rounded_rectangle([10 * k, 4 * k, (10 + bw) * k, (4 + bh) * k], radius=bh * k // 2, fill=255)
    # the tail: a curved wedge out of the bottom left of the body, pointing down-left
    tx = 10 + int(bh * 0.42)
    by = 4 + bh
    pts = []
    for i in range(21):  # outer curve
        t = i / 20
        pts.append(((tx + 70 - 70 * t - 60 * t * t) * k, (by - 40 + (tail + 30) * t) * k))
    for i in range(21):  # inner curve back up
        t = 1 - i / 20
        pts.append(((tx - 60 * t * t + 20 - 10 * t) * k, (by - 40 + (tail + 30) * t * 0.55 + 30 * t) * k))
    md.polygon(pts, fill=255)
    m = m.resize((pw, ph), Image.LANCZOS).filter(ImageFilter.GaussianBlur(0.6))

    yy, xx = np.mgrid[0:ph, 0:pw].astype(np.float32)
    paper = np.array([240, 226, 192], np.float32) * (1.06 - 0.16 * (yy / ph))[..., None]
    rng = np.random.default_rng(3)
    paper += rng.normal(0, 4, (ph, pw, 1))
    stains = Image.effect_noise((pw // 10, ph // 10), 50).resize((pw, ph), Image.BICUBIC)
    paper *= (0.9 + 0.1 * np.asarray(stains, np.float32) / 255)[..., None]
    edge = np.asarray(m.filter(ImageFilter.GaussianBlur(16)), np.float32) / 255
    paper *= (0.5 + 0.5 * np.clip(edge * 1.7 - 0.35, 0, 1))[..., None]  # scorched edge
    plate = Image.fromarray(np.clip(paper, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    plate.putalpha(m)

    # brass rim with rivets along the top and bottom of the body
    ring = ImageChops.subtract(m, m.filter(ImageFilter.MinFilter(11)))
    rim = Image.new("RGBA", (pw, ph), (184, 136, 62, 255))
    plate.paste(rim, mask=ring)
    d = ImageDraw.Draw(plate)
    for x in range(10 + bh // 2, 10 + bw - bh // 2 + 1, 64):
        rivet(d, x, 15, 4)
        if x > tx + 90:
            rivet(d, x, 4 + bh - 11, 4)

    ink = (46, 26, 12)
    x0, y0 = 10 + padx, 4 + pady
    for i, l in enumerate(lines):
        d.text((x0 + 1, y0 + i * lh + 1), l, font=font, fill=(120, 90, 50))  # pressed-in edge
        d.text((x0, y0 + i * lh), l, font=font, fill=ink)
    return plate


def make():
    base = background()

    # big gears in the corners, cut off by the screen edge
    paste_shadowed(base, gear(460, 22, (176, 126, 56, 255), (122, 82, 34, 255), seed=0.2), (-170, -170))
    paste_shadowed(base, gear(250, 14, (150, 84, 44, 255), (104, 56, 28, 255), seed=0.5), (205, 40))
    paste_shadowed(base, gear(520, 26, (168, 118, 52, 255), (116, 78, 32, 255), seed=0.1), (W - 330, H - 330))
    paste_shadowed(base, gear(230, 12, (158, 92, 48, 255), (110, 62, 30, 255), seed=0.9), (W - 430, H - 210))
    paste_shadowed(base, gear(190, 12, (182, 134, 60, 255), (124, 86, 36, 255), seed=0.3), (W - 175, 30))
    paste_shadowed(base, gear(150, 10, (150, 84, 44, 255), (104, 56, 28, 255), seed=0.0), (40, H - 210))

    # pipes along the bottom and a gauge
    pipe_h(base, H - 88, 0, W - 380, 34, joints=[180, 520, 760])
    gauge(base, 640, 668, 48, 300)

    # the logo
    lines = logo_lines()
    font = ImageFont.truetype(MONO, 23, index=1)  # Menlo Bold
    cw = int(font.getlength("M"))
    lh = 30
    plate = bubble_plate(lines, font, cw, lh)
    x = (W - plate.width) // 2
    y = int(H * 0.44 - plate.height / 2)
    paste_shadowed(base, plate, (x, y), blur=18, offset=(12, 16), alpha=200)

    # an engraved caption under it
    small = ImageFont.truetype(MONO, 15)
    cap = "~ XDJ-AZ ~ MALA IDEA STEAM WORKS ~ EST. 2026 ~"
    d = ImageDraw.Draw(base)
    cx = (W - small.getlength(cap)) / 2
    cy = y + plate.height + 18
    d.text((cx + 1, cy + 1), cap, font=small, fill=(20, 12, 6))
    d.text((cx, cy), cap, font=small, fill=(206, 160, 86))

    return base.convert("RGB")


def make_icon():
    """256x256 icon for the Mod Index: a gear and the bubble with an ASCII "MI"."""
    n = 256
    yy, xx = np.mgrid[0:n, 0:n].astype(np.float32)
    bg = np.array([46, 30, 17], np.float32) * (1.1 - 0.5 * np.hypot(xx - n / 2, yy - n / 2)[..., None] / n)
    icon = Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8), "RGB").convert("RGBA")
    paste_shadowed(icon, gear(300, 18, (176, 126, 56, 255), (122, 82, 34, 255), seed=0.2), (20, 20))
    font = ImageFont.truetype(MONO, 22, index=1)
    plate = bubble_plate(ascii_word("MI"), font, int(font.getlength("M")), 28)
    scale = 220 / plate.width
    plate = plate.resize((220, int(plate.height * scale)), Image.LANCZOS)
    paste_shadowed(icon, plate, (18, 40), blur=8, offset=(5, 7), alpha=200)
    return icon.convert("RGB")


def write_miw(img, path):
    q = img.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG)
    pal = (q.getpalette() or [])[: 256 * 3]
    pal += [0] * (256 * 3 - len(pal))
    with open(path, "wb") as f:
        f.write(b"MIW1" + struct.pack("<HH", img.width, img.height))
        f.write(bytes(pal))
        f.write(q.tobytes())
    return q


if __name__ == "__main__":
    random.seed(1)
    img = make()
    os.makedirs(os.path.join(ROOT, "images"), exist_ok=True)
    img.save(os.path.join(ROOT, "images", "wallpaper.png"), optimize=True)
    q = write_miw(img, os.path.join(ROOT, "mala-idea-wallpaper", "assets", "wallpaper.miw"))
    q.convert("RGB").save(os.path.join(ROOT, "images", "wallpaper-on-deck.png"), optimize=True)
    make_icon().save(os.path.join(ROOT, "mala-idea-wallpaper", "icon.png"))
    print("\n".join(logo_lines()))
    print("wrote images/wallpaper.png and mala-idea-wallpaper/assets/wallpaper.miw")
