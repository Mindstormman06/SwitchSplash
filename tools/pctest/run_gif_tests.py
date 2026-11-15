"""PC tests for the GIF decoder + compositor (gif_stream.c, gif_canvas.c) against PIL.

Usage: python run_gif_tests.py <work_dir>
"""
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "source"))
WORK = sys.argv[1]
os.makedirs(WORK, exist_ok=True)
EXE = os.path.join(WORK, "test_gif.exe")
subprocess.run(["gcc", "-O2", "-Wall", "-Wextra", "-I", SRC, os.path.join(HERE, "test_gif.c"),
                os.path.join(SRC, "gif_stream.c"), os.path.join(SRC, "gif_canvas.c"), "-o", EXE], check=True)

OW, OH = 640, 360
rng = np.random.default_rng(3)
failures = []


def q4(v):
    return (v.astype(np.int32) * 15) // 255  # no dithering


def pil_frames(path, bg):
    """PIL's composited RGBA frames, composited over bg, as 4-bit RGB."""
    im = Image.open(path)
    out = []
    for i in range(getattr(im, "n_frames", 1)):
        im.seek(i)
        rgba = np.asarray(im.convert("RGBA")).astype(np.float64)
        a = rgba[..., 3:4] / 255.0
        comp = rgba[..., :3] * a + np.array(bg, np.float64) * (1 - a)
        out.append(q4(np.round(comp)))
    return out


def expected_canvas(frame4, gw, gh, bg4):
    canvas = np.empty((OH, OW, 3), np.int32)
    canvas[:] = bg4
    if gw <= OW and gh <= OH:
        iw, ih = gw, gh
    elif gw * OH >= gh * OW:
        iw, ih = OW, (gh * OW + gw // 2) // gw
    else:
        ih, iw = OH, (gw * OH + gh // 2) // gh
    ox, oy = (OW - iw) // 2, (OH - ih) // 2
    sx = (np.arange(iw) * gw) // iw
    sy = (np.arange(ih) * gh) // ih
    canvas[oy:oy + ih, ox:ox + iw] = frame4[sy][:, sx]
    return canvas


def run(name, frames_rgb_or_imgs, bg=(0, 0, 0), **save_kw):
    path = os.path.join(WORK, name)
    imgs = frames_rgb_or_imgs
    imgs[0].save(path, save_all=True, append_images=imgs[1:], **save_kw)
    return check(name, path, bg)


def check(name, path, bg=(0, 0, 0)):
    raw = path + ".raw"
    bghex = "%02X%02X%02X" % bg
    p = subprocess.run([EXE, path, raw, str(OW), str(OH), bghex], capture_output=True, text=True)
    lines = p.stdout.strip().splitlines()
    if p.returncode != 0 or not lines or lines[0].startswith("ERR"):
        failures.append(f"{name}: {p.stdout.strip()} {p.stderr.strip()}")
        print(f"  {name:32s} FAIL {p.stdout.strip()[:120]}")
        return
    gw, gh, loop = map(int, lines[0].split())
    meta = [list(map(int, l.split())) for l in lines[1:]]
    data = np.fromfile(raw, "<u2").reshape(-1, OH, OW)
    got = np.stack([(data >> (4 * i)) & 0xF for i in range(3)], -1).astype(np.int32)
    exp = pil_frames(path, bg)
    bg4 = q4(np.array(bg))
    ok = len(meta) == len(exp) and got.shape[0] == len(exp) + 1
    worst = 0
    for i, e in enumerate(exp[: len(meta)]):
        diff = int(np.count_nonzero(np.any(got[i] != expected_canvas(e, gw, gh, bg4), -1)))
        worst = max(worst, diff)
    ok = ok and worst == 0
    # The rewind replay must equal frame 0.
    ok = ok and np.array_equal(got[-1], got[0])
    im = Image.open(path)
    pil_loop = im.info.get("loop", None)
    loop_ok = (loop == -1 and pil_loop is None) or (pil_loop is not None and loop == pil_loop)
    ok = ok and loop_ok
    delays = [m[0] * 10 for m in meta]
    print(f"  {name:32s} {'OK' if ok else 'FAIL'}  {gw}x{gh} frames={len(meta)} loop={loop} "
          f"worst_diff_px={worst} delays_ms={delays[:4]}{'...' if len(delays) > 4 else ''}")
    if not ok:
        failures.append(f"{name}: frames {len(meta)} vs {len(exp)}, worst {worst}, loop {loop} vs {pil_loop}")


def bouncing(w, h, n, mode="RGB"):
    frames = []
    for i in range(n):
        a = np.zeros((h, w, 3), np.uint8)
        a[...] = (20, 40, 80)
        x = (i * 7) % (w - 20)
        a[h // 3:h // 3 + 20, x:x + 20] = (255, 200, 0)
        a[: h // 6, :] = ((i * 30) % 256, 100, 200)
        frames.append(Image.fromarray(a, "RGB"))
    return frames


print("gif tests:")
# 1. Simple full frames, small (1:1 centred), infinite loop, per-frame delays.
run("bounce_200x120.gif", bouncing(200, 120, 12), duration=[40, 60] * 6, loop=0)
# 2. Exactly the canvas size, play once (no loop block).
run("full_640x360.gif", bouncing(640, 360, 5), duration=50)
# 3. Larger than the canvas -> nearest downscale.
run("big_1280x720.gif", bouncing(1280, 720, 4), duration=100, loop=0)
# 4. Odd aspect, scaled (tall).
run("tall_300x800.gif", bouncing(300, 800, 3), duration=100, loop=3)
# 5. Local palettes: every frame a different random palette image.
pal_frames = []
for i in range(6):
    a = rng.integers(0, 256, (90, 160, 3), dtype=np.uint8)
    pal_frames.append(Image.fromarray(a, "RGB").quantize(64 + i * 30))
run("local_palettes.gif", pal_frames, duration=80, loop=0)
# 6. Transparency + disposal 2 (background) with partial frames (PIL optimises rects).
tf = []
for i in range(8):
    im = Image.new("RGBA", (160, 100), (0, 0, 0, 0))
    a = np.asarray(im).copy()
    x = 10 + i * 15
    a[30:60, x:x + 25] = (255, 50, 50, 255)
    tf.append(Image.fromarray(a, "RGBA"))
run("transparent_dispose2.gif", tf, bg=(16, 24, 48), duration=60, loop=0, disposal=2)
# 7. Transparency without disposal (trails accumulate).
run("transparent_keep.gif", tf, bg=(0, 0, 0), duration=60, loop=0, disposal=1)
# 8. Disposal 3 (restore previous).
run("dispose_previous.gif", tf, bg=(0, 0, 0), duration=60, loop=0, disposal=3)
# 9. Interlaced.
run("interlaced.gif", bouncing(120, 77, 4), duration=100, loop=0, interlace=True)
# 10. Optimized partial-rectangle frames.
run("optimized.gif", bouncing(320, 180, 10), duration=33, loop=0, optimize=True)


# Hand-made: 1x1 GIF87a, and an LZW stream with codes up to 12 bits (large noisy image).
def write_gif87(path):
    data = (b"GIF87a" + struct.pack("<HHBBB", 1, 1, 0x80, 0, 0) + b"\x00\x00\x00\xff\xff\xff" +
            b"\x2c" + struct.pack("<HHHHB", 0, 0, 1, 1, 0) + b"\x02\x02\x44\x01\x00" + b"\x3b")
    open(path, "wb").write(data)


write_gif87(os.path.join(WORK, "tiny87a.gif"))
check("tiny87a.gif", os.path.join(WORK, "tiny87a.gif"))
noise = Image.fromarray(rng.integers(0, 256, (360, 640, 3), dtype=np.uint8), "RGB").quantize(256)
noise.save(os.path.join(WORK, "noise_12bit.gif"))
check("noise_12bit.gif", os.path.join(WORK, "noise_12bit.gif"))

print("error handling:")
def expect_err(name, data):
    path = os.path.join(WORK, name)
    open(path, "wb").write(data)
    p = subprocess.run([EXE, path, path + ".raw"], capture_output=True, text=True)
    ok = p.returncode == 1 and "ERR" in p.stdout and p.returncode < 128
    print(f"  {name:32s} {'OK' if ok else 'FAIL'} ({p.stdout.strip()[:80]})")
    if not ok:
        failures.append(name)

good = open(os.path.join(WORK, "bounce_200x120.gif"), "rb").read()
expect_err("not_gif.gif", b"\x89PNG\r\n\x1a\n not a gif")
expect_err("empty.gif", b"")
expect_err("huge.gif", b"GIF89a" + struct.pack("<HH", 5000, 100) + b"\x00\x00\x00;")
# Truncated mid-stream: decodes what's there, must not crash (exit 0 or 1 both fine).
path = os.path.join(WORK, "truncated.gif")
open(path, "wb").write(good[: len(good) // 2])
p = subprocess.run([EXE, path, path + ".raw"], capture_output=True, text=True)
ok = p.returncode in (0, 1)
print(f"  {'truncated.gif':32s} {'OK' if ok else 'FAIL'} (rc={p.returncode})")
if not ok:
    failures.append("truncated")
# Corrupted LZW data.
bad = bytearray(good)
for i in range(len(bad) // 3, len(bad) // 3 + 200):
    bad[i] ^= 0x5A
path = os.path.join(WORK, "corrupt.gif")
open(path, "wb").write(bytes(bad))
p = subprocess.run([EXE, path, path + ".raw"], capture_output=True, text=True)
ok = p.returncode in (0, 1)
print(f"  {'corrupt.gif':32s} {'OK' if ok else 'FAIL'} (rc={p.returncode})")
if not ok:
    failures.append("corrupt")

print()
if failures:
    print("FAILURES:", *failures, sep="\n  ")
    sys.exit(1)
print("ALL TESTS PASSED")
