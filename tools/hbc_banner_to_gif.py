"""Builds an animated hbl/ splash.gif from the Homebrew Channel's open-source banner assets.

    python -P tools/hbc_banner_to_gif.py <hbc repo>/channel <out dir> [seconds] [fps]

Source: https://github.com/fail0verflow/hbc (GPL-2.0), channel/banner/banner/*.png and the
animation script channel/banner/banner/mkbanner.py.

This replays the banner's real animation (mkbanner.py's BRLAN keyframes, Hermite-interpolated
like the Wii does) instead of approximating it:
  * the water rises in over the first 160 frames (60 fps banner time),
  * the bubble burst at frame 190 and the steady stream after it (same random.seed(1) and
    call order as mkbanner.py, so the same bubbles),
  * the white flash at frame 244 that reveals the title, which then bobs up and down,
  * the waves sliding and bobbing at their own periods.
The GIF starts at banner frame 0, in sync with splash.wav from hbc_banner_to_splash.py (the
music's loop part kicks in at 4.21 s, right on the flash at 4.07 s). It plays once and holds
its last frame. Frames are rendered at 1280x720 and box-filtered to 640x360 (SwitchSplash
draws GIFs at 640x360), so slow motion still moves smoothly in half-pixel steps.

Default: 12.6 s (the length of splash.wav) at 30 fps.
"""
import math
import os
import random
import sys

from PIL import Image

SRC, OUT = sys.argv[1], sys.argv[2]
SECONDS = float(sys.argv[3]) if len(sys.argv) > 3 else 12.6
FPS = int(sys.argv[4]) if len(sys.argv) > 4 else 30
BAN = os.path.join(SRC, "banner", "banner")
os.makedirs(OUT, exist_ok=True)

CANVAS_W, CANVAS_H = 810, 456
OUT_W, OUT_H = 640, 360
W, H = OUT_W * 2, OUT_H * 2  # render size
S = W / CANVAS_W

# ---------------------------------------------------------------- BRLAN interpolation --


def hermite(t, t0, p0, m0, t1, p1, m1):
    """BRLAN Hermite segment; slopes are in value units per frame."""
    d = t1 - t0
    if d <= 0:
        return p1
    u = (t - t0) / d
    u2, u3 = u * u, u * u * u
    return ((2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 * d
            + (-2 * u3 + 3 * u2) * p1 + (u3 - u2) * m1 * d)


def keys(t, triplets):
    """Evaluates a keyframe track [(frame, value, slope), ...] (clamped outside it)."""
    if t <= triplets[0][0]:
        return triplets[0][1]
    for (f0, v0, s0), (f1, v1, s1) in zip(triplets, triplets[1:]):
        if t < f1:
            return hermite(t, f0, v0, s0, f1, v1, s1)
    return triplets[-1][1]


FAKE_START, LOOP_END = -60 * 10, 60 * 22


def repsimple(t, n, a, sa, b, sb, start=FAKE_START, end=LOOP_END):
    """Alameda's repsimple: n full a->b->a cycles between start and end."""
    half = (end - start) / n / 2
    k = math.floor((t - start) / half)
    t0 = start + k * half
    if k % 2 == 0:
        return hermite(t, t0, a, sa, t0 + half, b, sb)
    return hermite(t, t0, b, sb, t0 + half, a, sa)


# -------------------------------------------------------------------------- pictures --


def tex(name):
    return Image.open(os.path.join(BAN, name)).convert("RGBA")


def picture(texture, w, h, repeat_s=1, t_scale=1, extend_down=0):
    """A pane's picture at render scale: the texture repeated `repeat_s` times across and
    filling 1/t_scale of the height, the last row clamped below it (mkbanner.py's wrap
    modes). `extend_down` adds clamped height so the water reaches the bottom edge."""
    pw = int(round(w * S))
    ph_tex = int(round(h / t_scale * S))
    ph = int(round((h + extend_down) * S))
    one = texture.resize((max(1, int(round(pw / repeat_s))), ph_tex), Image.LANCZOS)
    img = Image.new("RGBA", (pw, ph))
    x = 0
    while x < pw:
        img.paste(one, (x, 0))
        x += one.width
    if ph > ph_tex:
        last = img.crop((0, ph_tex - 1, pw, ph_tex)).resize((pw, ph - ph_tex), Image.NEAREST)
        img.paste(last, (0, ph_tex))
    return img


def place(canvas, img, cx, cy, w, h, alpha=1.0):
    """Composites a pane (centre cx, cy in layout units, Y up; w x h layout units)."""
    if alpha <= 0:
        return
    left = int(round(W / 2 + (cx - w / 2) * S))
    top = int(round(H / 2 - (cy + h / 2) * S))
    x0, y0 = max(0, left), max(0, top)
    x1, y1 = min(W, left + img.width), min(H, top + img.height)
    if x0 >= x1 or y0 >= y1:
        return
    src = img.crop((x0 - left, y0 - top, x1 - left, y1 - top))
    if alpha < 1.0:
        a = src.getchannel("A").point(lambda v: int(v * alpha + 0.5))
        src.putalpha(a)
    canvas.alpha_composite(src, (x0, y0))


EXTRA = 520  # water extends this far below its pane, so it covers the bottom while rising
P = {
    "wavea": picture(tex("banner_wavea.png"), 3072, 384, 3, 6, EXTRA),
    "waveb": picture(tex("banner_waveb.png"), 3072, 384, 3, 6, EXTRA),
    "wave1a": picture(tex("banner_wave1a.png"), 382, 32),
    "wave1b": picture(tex("banner_wave1b.png"), 527, 37),
    "shadow": picture(tex("banner_shape2.png"), 644, 28),
    "fade": picture(tex("banner_fade.png"), CANVAS_W, 256, extend_down=EXTRA),
    "title": picture(tex("banner_title.png"), 400, 180),
}

# --------------------------------------------- bubbles (mkbanner.py, same RNG order) --

random.seed(1)
TYPES = [("abubble1", 48), ("abubble2", 32), ("abubble3", 16), ("abubble4", 24), ("abubble5", 32),
         ("abubble6", 16), ("bbubble1", 48), ("cbubble1", 64), ("cbubble2", 16)]


class Bubble:
    def __init__(self, start, length=None, x=None, xp=None):
        self.x = random.uniform(-CANVAS_W / 2 - 64, CANVAS_W / 2 + 64) if x is None else x
        self.xp = random.uniform(0, 32) if xp is None else xp
        self.length = random.uniform(50, 170) if length is None else length
        self.start = start
        self.end = start + self.length
        self.type = None


def choosetype():
    opt = random.uniform(0, len(TYPES))
    for i in range(len(TYPES)):
        if 1 > opt:
            return i
        opt -= 1
    return i


bubbles = []
for _ in range(100):  # the burst
    b = Bubble(190)
    b.type = choosetype()
    bubbles.append(b)
for _ in range(280):  # the steady stream
    b = Bubble(random.uniform(190, LOOP_END))
    b.type = choosetype()
    bubbles.append(b)
for b in bubbles:  # mkbanner.py's (skipping) removal loop, kept as-is for the same set
    if b.end > LOOP_END:
        bubbles.remove(b)
bubbles.sort(key=lambda b: b.start)
BUB = {name: picture(tex(name + ".png"), size, size) for name, size in TYPES}

# ---------------------------------------------------------------------------- frames --


def render(t):
    """Banner frame t (60 fps) at render size."""
    c = Image.new("RGBA", (W, H), (255, 255, 255, 255))
    wy = keys(t, [(0, -320, 0), (160, 0, 0)])  # water pane rises in

    def water(name, cx, cy, w, h, extend=0):
        place(c, P[name], cx, cy + wy - extend / 2, w, h + extend)

    water("wavea", repsimple(t, 4, -300, 0, 300, 0), repsimple(t, 6, 35, 0, 55, 0), 3072, 384, EXTRA)
    water("waveb", repsimple(t, 4, -300, 2.0, 300, 2.0), repsimple(t, 8, 45, 0, 55, 0), 3072, 384, EXTRA)
    water("wave1a", repsimple(t, 4, -200, 4.0, 200, 4.0), repsimple(t, 6, 160, 0.2, 180, 0.2), 382, 32)
    water("wave1b", repsimple(t, 4, 200, 3.2, 400, 3.2), repsimple(t, 6, 170, 0.2, 183, 0.2), 527, 37)
    water("wave1b", repsimple(t, 4, -400, 3.7, -200, 3.7), repsimple(t, 6, 165, 0.2, 185, 0.2), 527, 37)
    water("shadow", repsimple(t, 4, -180, 1.4, 100, 1.4), repsimple(t, 6, 150, 0.2, 155, 0.2), 644, 28)
    water("fade", 0, 8, CANVAS_W, 256, EXTRA)

    for b in bubbles:  # inside the water pane
        if b.start <= t < b.end:
            name, size = TYPES[b.type]
            y = hermite(t, b.start, -150, 1, b.end, 170, 1)
            fadepoint = b.start + b.length * 0.7
            a = 1.0 if t < fadepoint else hermite(t, fadepoint, 1.0, 0, b.end, 0.0, 0)
            place(c, BUB[name], b.x, y + wy, size, size, a)

    title_a = keys(t, [(0, 0, 0), (243, 0, 0), (244, 1, 0)])
    place(c, P["title"], 0, repsimple(t, 12, 32, 0, 20, 0), 400, 180, title_a)

    boom = keys(t, [(0, 0, 0), (243, 0, 0), (244, 1, 0), (246, 1, 0), (256, 0, 0)])
    if boom > 0:
        c.alpha_composite(Image.new("RGBA", (W, H), (255, 255, 255, int(round(boom * 255)))))
    return c.convert("RGB").resize((OUT_W, OUT_H), Image.BOX)


n = int(round(SECONDS * FPS))
frames = []
for i in range(n):
    frames.append(render(i * 60 / FPS))
    if i % 50 == 0:
        print(f"  frame {i}/{n}", flush=True)

# One global palette from a spread of frames, no dithering: dithering would shimmer from
# frame to frame and bloat the file (the console dithers to RGBA4444 itself anyway).
sample_idx = [int(n * k / 12) for k in range(12)]
strip = Image.new("RGB", (OUT_W, OUT_H * len(sample_idx)))
for k, i in enumerate(sample_idx):
    strip.paste(frames[i], (0, OUT_H * k))
pal = strip.quantize(256, method=Image.Quantize.MEDIANCUT)
q = [f.quantize(palette=pal, dither=Image.Dither.NONE) for f in frames]

# Exact frame timing in centiseconds (e.g. 3,3,4 for 30 fps), so it stays in sync with the
# jingle.
cs = [round((i + 1) * 100 / FPS) - round(i * 100 / FPS) for i in range(n)]
path = os.path.join(OUT, "splash.gif")
q[0].save(path, save_all=True, append_images=q[1:], duration=[c * 10 for c in cs],
          optimize=False, disposal=1)  # no loop extension: plays once, holds the last frame
print(f"wrote {path} ({n} frames, {OUT_W}x{OUT_H}, {os.path.getsize(path) / 1e6:.2f} MB)")
