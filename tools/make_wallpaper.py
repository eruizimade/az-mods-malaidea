#!/usr/bin/env python3
"""Draws the Mala Idea wallpaper for the XDJ-AZ screen (1280x800): the logo in ASCII on black.

    python3 -m pip install pillow numpy
    python3 tools/make_wallpaper.py

Writes:
    images/wallpaper.png                          a still of the logo traced in ASCII
    mala-idea-wallpaper/assets/corrent.mic        what the mod animates on the deck (write_corrent)

The animation itself is drawn by the mod, frame by frame; preview/fons-animat.html shows the
same thing in a browser.
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


LOGO = os.path.join(ROOT, "tools", "mala-idea-logo.png")


def ascii_logo(cols, font, cw, lh, color=(255, 255, 255)):
    """The real logo (tools/mala-idea-logo.png) traced into ASCII: the bubble's outline in
    line characters that follow its direction, the "Mala Idea" letters filled by density.
    Returns a transparent layer with the characters."""
    src = Image.open(LOGO).convert("L")
    a = np.asarray(src, np.float32) / 255
    ys, xs = np.where(a > 0.5)
    m = 12
    a = a[ys.min() - m:ys.max() + m, xs.min() - m:xs.max() + m]
    bubble = Image.fromarray((a > 0.5).astype(np.uint8) * 255)
    # the bubble's silhouette: the white shape with the letters' holes filled
    filled = bubble.copy()
    ImageDraw.floodfill(filled, (0, 0), 128)                  # everything outside the bubble
    filled = filled.point(lambda v: 0 if v == 128 else 255)
    fill = np.asarray(filled, np.float32) / 255
    inner = np.asarray(filled.filter(ImageFilter.MinFilter(9)), np.float32) / 255
    text = (fill > 0.5) & (inner > 0.5) & (a < 0.5)          # dark ink inside the bubble
    edge = (fill > 0.5) & ~(np.asarray(filled.filter(ImageFilter.MinFilter(3))) > 128)
    # the outline's direction, from the gradient of the blurred silhouette
    soft = np.asarray(filled.filter(ImageFilter.GaussianBlur(6)), np.float32)
    gy, gx = np.gradient(soft)

    h, w = a.shape
    rows = int(round(cols * (h / w) * (cw / lh)))
    sx, sy = w / cols, h / rows
    ramp = " .:+#@"
    out = []
    for r in range(rows):
        line = ""
        for c in range(cols):
            y0, y1 = int(r * sy), int((r + 1) * sy)
            x0, x1 = int(c * sx), int((c + 1) * sx)
            t = text[y0:y1, x0:x1].mean()
            e = edge[y0:y1, x0:x1]
            if t > 0.12:
                line += ramp[min(len(ramp) - 1, int(t * (len(ramp) - 1) + 0.5))]
            elif e.mean() > 0.05:
                ey, ex = np.where(e)
                ny = gy[y0:y1, x0:x1][ey, ex].mean()
                nx = gx[y0:y1, x0:x1][ey, ex].mean()
                ang = (math.degrees(math.atan2(ny, nx)) + 180) % 180  # the normal
                if ang < 22.5 or ang >= 157.5:
                    ch = "|"
                elif ang < 67.5:
                    ch = "/"
                elif ang < 112.5:
                    ch = "-" if ny < 0 else "_"   # the top edge and the bottom edge
                else:
                    ch = "\\"
                line += ch
            else:
                line += " "
        out.append(line.rstrip())

    lw, lhh = cols * cw + 40, rows * lh + 40
    layer = Image.new("RGBA", (lw, lhh), (0, 0, 0, 0))
    d = ImageDraw.Draw(layer)
    for i, l in enumerate(out):
        d.text((20, 20 + i * lh), l, font=font, fill=color + (255,))
    res = layer
    return res, out


# ---- Corrent: the animated wallpaper -----------------------------------------------------

CELL_W, CELL_H = 6, 12                       # one character on the deck: 6x12 px
RAMP = " .:-=+*"                             # the bubble's inside, by the wave's height
DENSE = "@#%&8$"                             # the letters
DRIFT = "!#$%&*+-:;=?@[]<>/\\|^~01MALIDE"    # the loose characters outside the bubble
EDGES = "|/\\-_"


def corrent_grid():
    """The logo as screen cells (213 x 66 of 6x12 px): 0 outside, 1 inside the bubble, 2 its
    outline (with the line character that follows it), 3 the letters."""
    src = Image.open(LOGO).convert("L")
    a = np.asarray(src, np.float32) / 255
    ys, xs = np.where(a > 0.5)
    m = 12
    a = a[ys.min() - m:ys.max() + m, xs.min() - m:xs.max() + m]
    # a copy: an image made straight from an array can share its memory, and the flood fill
    # then leaves the outside unmarked
    filled = Image.fromarray((a > 0.5).astype(np.uint8) * 255).copy()
    ImageDraw.floodfill(filled, (0, 0), 128)
    filled = filled.point(lambda v: 0 if v == 128 else 255)
    fill = np.asarray(filled, np.float32) / 255
    inner = np.asarray(filled.filter(ImageFilter.MinFilter(9)), np.float32) / 255
    text = (fill > 0.5) & (inner > 0.5) & (a < 0.5)
    edge = (fill > 0.5) & ~(np.asarray(filled.filter(ImageFilter.MinFilter(3))) > 128)
    soft = np.asarray(filled.filter(ImageFilter.GaussianBlur(6)), np.float32)
    gy, gx = np.gradient(soft)

    gc, gr = W // CELL_W, H // CELL_H
    h, w = a.shape
    cols = 196
    rows = int(round(cols * (h / w) * (CELL_W / CELL_H)))
    sx, sy = w / cols, h / rows
    ox, oy = (gc - cols) // 2, (gr - rows) // 2
    kind = np.zeros((gr, gc), np.uint8)
    edge_ch = np.full((gr, gc), " ")
    for r in range(rows):
        for c in range(cols):
            y0, y1 = int(r * sy), int((r + 1) * sy)
            x0, x1 = int(c * sx), int((c + 1) * sx)
            e = edge[y0:y1, x0:x1]
            if text[y0:y1, x0:x1].mean() > 0.12:
                kind[r + oy, c + ox] = 3
            elif e.mean() > 0.05:
                ey, ex = np.where(e)
                ny = gy[y0:y1, x0:x1][ey, ex].mean()
                nx = gx[y0:y1, x0:x1][ey, ex].mean()
                ang = (math.degrees(math.atan2(ny, nx)) + 180) % 180
                if ang < 22.5 or ang >= 157.5:
                    ch = "|"
                elif ang < 67.5:
                    ch = "/"
                elif ang < 112.5:
                    ch = "-" if ny < 0 else "_"
                else:
                    ch = "\\"
                kind[r + oy, c + ox] = 2
                edge_ch[r + oy, c + ox] = ch
            elif fill[y0:y1, x0:x1].mean() > 0.5:
                kind[r + oy, c + ox] = 1
    return kind, edge_ch


def write_corrent(path):
    """assets/corrent.mic, everything the mod needs to draw Corrent with no font of its own:
        "MIC1"  u16 cols  u16 rows  u8 cell_w  u8 cell_h  u8 n_glyphs
        n_glyphs characters (ASCII), then each one's coverage, cell_w*cell_h bytes (0..255)
        cols*rows cell kinds (0 outside, 1 bubble, 2 outline, 3 letter)
        cols*rows outline characters (a glyph index, 255 for none)"""
    kind, edge_ch = corrent_grid()
    chars = "".join(sorted(set(RAMP + DENSE + DRIFT + EDGES) - {" "}))
    font = ImageFont.truetype(MONO, 10, index=1)  # Menlo Bold, as in the preview
    cover = b""
    for ch in chars:
        im = Image.new("L", (CELL_W, CELL_H), 0)
        ImageDraw.Draw(im).text((0, 0), ch, font=font, fill=255)
        cover += im.tobytes()
    gr, gc = kind.shape
    idx = np.full((gr, gc), 255, np.uint8)
    for r in range(gr):
        for c in range(gc):
            if edge_ch[r, c] != " ":
                idx[r, c] = chars.index(edge_ch[r, c])
    with open(path, "wb") as f:
        f.write(b"MIC1" + struct.pack("<HHBBB", gc, gr, CELL_W, CELL_H, len(chars)))
        f.write(chars.encode("ascii") + cover + kind.tobytes() + idx.tobytes())
    return chars


def make():
    """Black, with the logo traced into white ASCII in the middle. Nothing else."""
    base = Image.new("RGBA", (W, H), (0, 0, 0, 255))
    font = ImageFont.truetype(MONO, 10, index=1)  # Menlo Bold
    logo, _ = ascii_logo(196, font, int(round(font.getlength("M"))), 12)
    base.alpha_composite(logo, ((W - logo.width) // 2, (H - logo.height) // 2))
    return base.convert("RGB")


def make_icon():
    """256x256 icon for the Mod Index: the ASCII logo on black."""
    n = 256
    icon = Image.new("RGBA", (n, n), (0, 0, 0, 255))
    font = ImageFont.truetype(MONO, 13, index=1)
    logo, _ = ascii_logo(90, font, int(round(font.getlength("M"))), 16)
    logo = logo.resize((244, int(logo.height * 244 / logo.width)), Image.LANCZOS)
    icon.alpha_composite(logo, (6, (n - logo.height) // 2))
    return icon.convert("RGB")




if __name__ == "__main__":
    random.seed(1)
    img = make()
    os.makedirs(os.path.join(ROOT, "images"), exist_ok=True)
    img.save(os.path.join(ROOT, "images", "wallpaper.png"), optimize=True)
    write_corrent(os.path.join(ROOT, "mala-idea-wallpaper", "assets", "corrent.mic"))
    make_icon().save(os.path.join(ROOT, "mala-idea-wallpaper", "icon.png"))
    print("\n".join(ascii_logo(196, ImageFont.truetype(MONO, 10, index=1), 6, 12)[1]))
    print("wrote images/wallpaper.png and mala-idea-wallpaper/assets/corrent.mic")
