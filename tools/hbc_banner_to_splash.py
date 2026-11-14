"""Builds an hbl/ splash from the Homebrew Channel's open-source banner assets.

    python -P tools/hbc_banner_to_splash.py <hbc repo>/channel <out dir>

Source: https://github.com/fail0verflow/hbc (GPL-2.0), files under channel/banner/:
  banner/*.png      layers of the animated channel banner (layout: banner/mkbanner.py)
  sound/wiibrew-banner-intro-part.wav, sound/wiibrew-banner-loop-part.wav  (32 kHz stereo)

The real banner is an animation (BRLYT/BRLAN). SwitchSplash shows a still image, so this
renders one frame from the steady loop section: the layout from mkbanner.py (810x456 canvas,
pane positions from the centre with Y pointing up), waves at mid-swing, and a deterministic
scatter of rising bubbles. Each layer is drawn straight at 1280x720 scale (x1.58) instead of
upscaling the finished canvas.

Writes <out dir>/splash.png and <out dir>/splash.wav (intro + one pass of the loop,
resampled to 48 kHz, with a short fade at the very end).
"""
import os
import random
import sys
import wave

import numpy as np
from PIL import Image
from scipy.signal import resample_poly

SRC, OUT = sys.argv[1], sys.argv[2]
BAN = os.path.join(SRC, "banner", "banner")
SND = os.path.join(SRC, "banner", "sound")
os.makedirs(OUT, exist_ok=True)

CANVAS_W, CANVAS_H = 810, 456
W, H = 1280, 720
S = W / CANVAS_W  # 1.580...


def tex(name):
    return Image.open(os.path.join(BAN, name)).convert("RGBA")


def to_screen(cx, cy, w, h):
    """Layout pane (centre x, y up) -> top-left pixel on the 1280x720 screen."""
    left = W / 2 + (cx - w / 2) * S
    top = H / 2 - (cy + h / 2) * S
    return int(round(left)), int(round(top))


def picture(texture, w, h, repeat_s=1, t_scale=1, extend_down=0):
    """Builds a pane's picture: the texture repeated `repeat_s` times across, and occupying
    1/t_scale of the height with the last row clamped below it (wrap modes from
    mkbanner.py). `extend_down` adds extra clamped height (layout units) so the water
    reaches the bottom of the screen."""
    pw = int(round(w * S))
    ph_tex = int(round(h / t_scale * S))
    ph = int(round((h + extend_down) * S))
    tile_w = pw / repeat_s
    one = texture.resize((max(1, int(round(tile_w))), ph_tex), Image.LANCZOS)
    img = Image.new("RGBA", (pw, ph))
    x = 0
    while x < pw:
        img.paste(one, (x, 0))
        x += one.width
    if ph > ph_tex:
        last = img.crop((0, ph_tex - 1, pw, ph_tex)).resize((pw, ph - ph_tex), Image.NEAREST)
        img.paste(last, (0, ph_tex))
    return img


canvas = Image.new("RGBA", (W, H), (255, 255, 255, 255))  # "background": white


def place(img, cx, cy, w, h, alpha=1.0):
    if alpha < 1.0:
        a = img.getchannel("A").point(lambda v: int(v * alpha))
        img = img.copy()
        img.putalpha(a)
    left, top = to_screen(cx, cy, w, h)
    if left >= W or top >= H or left + img.width <= 0 or top + img.height <= 0:
        return  # entirely off-screen
    layer = Image.new("RGBA", (W, H))
    layer.paste(img, (left, top))  # paste clips at every edge
    canvas.alpha_composite(layer)


# Water pane (at rest, Y=0 after its rise-in). Positions are mid-swing values of the
# oscillations in mkbanner.py's brlan (e.g. wavea Y 35..55 -> 45).
water_bottom_extra = 200  # clamp the water down past the bottom edge
place(picture(tex("banner_wavea.png"), 3072, 384, repeat_s=3, t_scale=6, extend_down=water_bottom_extra),
      0, 45 - water_bottom_extra / 2, 3072, 384 + water_bottom_extra)
place(picture(tex("banner_waveb.png"), 3072, 384, repeat_s=3, t_scale=6, extend_down=water_bottom_extra),
      150, 50 - water_bottom_extra / 2, 3072, 384 + water_bottom_extra)
place(picture(tex("banner_wave1a.png"), 382, 32), 0, 170, 382, 32)
place(picture(tex("banner_wave1b.png"), 527, 37), 300, 176, 527, 37)
place(picture(tex("banner_wave1b.png"), 527, 37), -300, 175, 527, 37)
place(picture(tex("banner_shape2.png"), 644, 28), -40, 152, 644, 28)
place(picture(tex("banner_fade.png"), CANVAS_W, 256, extend_down=water_bottom_extra),
      0, 8 - water_bottom_extra / 2, CANVAS_W, 256 + water_bottom_extra)

# Bubbles: same types, sizes and motion as mkbanner.py (rise from Y=-150 to 170, fade out
# over the last 30% of the trip), frozen at random points of their trip.
bubble_types = [("abubble1", 48), ("abubble2", 32), ("abubble3", 16), ("abubble4", 24), ("abubble5", 32),
                ("abubble6", 16), ("bbubble1", 48), ("cbubble1", 64), ("cbubble2", 16)]
rng = random.Random(1)
for _ in range(42):
    name, size = rng.choice(bubble_types)
    x = rng.uniform(-CANVAS_W / 2 - 64, CANVAS_W / 2 + 64)
    t = rng.uniform(0.0, 1.0)
    y = -150 + (170 - -150) * t
    alpha = 1.0 if t < 0.7 else max(0.0, 1.0 - (t - 0.7) / 0.3)
    place(picture(tex(name + ".png"), size, size), x, y, size, size, alpha)

# Title on top (alpha 255 in the loop; Y oscillates 20..32 -> 26).
place(picture(tex("banner_title.png"), 400, 180), 0, 26, 400, 180)

canvas.convert("RGB").save(os.path.join(OUT, "splash.png"), optimize=True)
print(f"wrote {OUT}/splash.png ({W}x{H})")

# --- Sound: intro + one pass of the loop, 32 kHz -> 48 kHz ---
parts = []
for name in ("wiibrew-banner-intro-part.wav", "wiibrew-banner-loop-part.wav"):
    with wave.open(os.path.join(SND, name)) as w:
        assert w.getsampwidth() == 2, name
        rate, ch = w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, ch).astype(np.float64)
        if ch == 1:
            x = np.repeat(x, 2, axis=1)
        parts.append((rate, x))
rate = parts[0][0]
assert all(r == rate for r, _ in parts)
pcm = np.concatenate([x for _, x in parts])
if rate != 48000:
    g = np.gcd(48000, rate)
    pcm = resample_poly(pcm, 48000 // g, rate // g, axis=0)
fade = 48000 // 5  # 200 ms, so playing to the very end never clicks
pcm[-fade:] *= np.linspace(1, 0, fade)[:, None]
out = np.clip(np.round(pcm), -32768, 32767).astype("<i2")
with wave.open(os.path.join(OUT, "splash.wav"), "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(48000)
    w.writeframes(out.tobytes())
peak = 20 * np.log10(max(np.abs(out).max(), 1) / 32768)
print(f"wrote {OUT}/splash.wav ({len(out) / 48000:.2f} s, 48 kHz stereo, peak {peak:.1f} dBFS)")
