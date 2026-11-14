"""Generates test splash images for hardware testing.

    python tools/make_test_splash.py <out_dir>

Writes:
  splash_1280x720.png   native size, shown 1:1
  splash_1920x1080.png  scaled down to 1280x720 by the sysmodule
  splash_800x800.png    smaller than the screen and square: centered, background around it

Each has a smooth gradient (to check dithering), a 1-pixel grid near the edges (to
check scaling and that nothing is cropped), and a label naming the file.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont


def make(w, h, label, path):
    y, x = np.mgrid[0:h, 0:w].astype(np.float32)
    r = 40 + 180 * (x / (w - 1))
    g = 30 + 120 * (y / (h - 1))
    b = 160 - 100 * (x / (w - 1)) * (y / (h - 1))
    img = Image.fromarray(np.stack([r, g, b], -1).clip(0, 255).astype(np.uint8), "RGB")

    d = ImageDraw.Draw(img)
    # Edge markers: a white border and corner squares, so cropping is obvious.
    d.rectangle([0, 0, w - 1, h - 1], outline=(255, 255, 255), width=max(2, w // 320))
    s = w // 16
    for cx, cy in ((0, 0), (w - s, 0), (0, h - s), (w - s, h - s)):
        d.rectangle([cx, cy, cx + s - 1, cy + s - 1], fill=(255, 255, 255))
    # Smooth grey ramp along the bottom: banding shows up here first.
    ramp = np.tile(np.linspace(0, 255, w, dtype=np.float32), (h // 10, 1)).astype(np.uint8)
    img.paste(Image.fromarray(np.stack([ramp] * 3, -1), "RGB"), (0, h - h // 10 - s))

    try:
        font = ImageFont.truetype("arialbd.ttf", h // 10)
        small = ImageFont.truetype("arial.ttf", h // 22)
    except OSError:
        font = small = ImageFont.load_default()
    d.text((w // 2, h // 2 - h // 12), "SwitchSplash", fill=(255, 255, 255), font=font, anchor="mm")
    d.text((w // 2, h // 2 + h // 20), label, fill=(255, 255, 255), font=small, anchor="mm")

    img.save(path, optimize=True)
    print(f"wrote {path} ({os.path.getsize(path) // 1024} KiB)")


out = sys.argv[1] if len(sys.argv) > 1 else "samples"
os.makedirs(out, exist_ok=True)
make(1280, 720, "1280x720 test image", os.path.join(out, "splash_1280x720.png"))
make(1920, 1080, "1920x1080 test image (scaled)", os.path.join(out, "splash_1920x1080.png"))
make(800, 800, "800x800 test image (centered)", os.path.join(out, "splash_800x800.png"))
