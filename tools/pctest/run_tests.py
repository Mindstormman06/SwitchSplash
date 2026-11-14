"""PC tests for the streaming PNG decoder + blitter.

Builds test_png.exe with the host gcc, generates PNGs covering every color type and bit
depth with PIL, and checks:
  1. decoded RGBA8 == PIL's RGBA conversion, byte for byte
  2. the 1280x720 RGBA4444 screen has the expected size, placement and background
  3. malformed / unsupported files fail cleanly instead of crashing
Writes preview PNGs of the screen output to the work dir for eyeballing.

Usage: python -I run_tests.py <work_dir>
"""
import os
import struct
import subprocess
import sys
import zlib

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "source"))
WORK = sys.argv[1]
os.makedirs(WORK, exist_ok=True)
EXE = os.path.join(WORK, "test_png.exe")

subprocess.run(
    ["gcc", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter", "-I", SRC,
     os.path.join(HERE, "test_png.c"), os.path.join(SRC, "png_stream.c"),
     os.path.join(SRC, "image_blit.c"), os.path.join(SRC, "miniz", "miniz_tinfl.c"),
     "-o", EXE],
    check=True,
)

rng = np.random.default_rng(1234)
failures = []


def gradient_rgba(w, h):
    y, x = np.mgrid[0:h, 0:w]
    r = (x * 255 // max(w - 1, 1)).astype(np.uint8)
    g = (y * 255 // max(h - 1, 1)).astype(np.uint8)
    b = ((x + y) % 256).astype(np.uint8)
    a = np.full((h, w), 255, np.uint8)
    img = np.stack([r, g, b, a], -1)
    # Some noise so every filter type gets exercised.
    img[..., :3] ^= rng.integers(0, 16, (h, w, 3), dtype=np.uint8)
    return img


def run(png_path, bg="000000"):
    raw = png_path + ".rgba"
    scr = png_path + ".scr"
    p = subprocess.run([EXE, png_path, raw, scr, bg], capture_output=True, text=True)
    return p, raw, scr


def check_decode(name, pil_img, **save_kw):
    path = os.path.join(WORK, name + ".png")
    pil_img.save(path, **save_kw)
    p, raw, scr = run(path)
    if p.returncode != 0 or p.stderr:
        failures.append(f"{name}: exit {p.returncode} out={p.stdout.strip()} err={p.stderr.strip()}")
        return None
    ref = np.asarray(Image.open(path).convert("RGBA"))
    # PIL's mode conversion for 16-bit grayscale ("I;16") clips instead of scaling, so
    # build that reference from the high byte ourselves.
    if pil_img.mode in ("I;16", "I;16B", "I"):
        hi = (np.asarray(Image.open(path)).astype(np.uint32) >> 8).astype(np.uint8)
        ref = np.stack([hi, hi, hi, np.full_like(hi, 255)], -1)
    got = np.fromfile(raw, np.uint8).reshape(ref.shape)
    diff = int(np.count_nonzero(got != ref))
    if diff:
        failures.append(f"{name}: {diff} bytes differ from PIL")
    print(f"  {name:28s} {pil_img.mode:6s} {pil_img.size}  {'OK' if not diff else 'DIFF ' + str(diff)}")
    return scr


def screen(scr):
    s = np.fromfile(scr, "<u2").reshape(720, 1280)
    return np.stack([(s >> (4 * i)) & 0xF for i in range(4)], -1).astype(np.uint8)


def save_preview(scr, name):
    s = screen(scr)
    Image.fromarray((s[..., :3] * 17).astype(np.uint8), "RGB").save(os.path.join(WORK, "preview_" + name + ".png"))


print("decode tests:")
base = gradient_rgba(1280, 720)
scr = check_decode("rgba8_1280x720", Image.fromarray(base, "RGBA"))
save_preview(scr, "rgba8_1280x720")
s = screen(scr)
if not (s[..., 3] == 15).all():
    failures.append("opaque 1280x720: not all alpha 15")

check_decode("rgb8", Image.fromarray(base[..., :3], "RGB"))
check_decode("rgb8_optimized", Image.fromarray(base[..., :3], "RGB"), optimize=True)
check_decode("gray8", Image.fromarray(base[..., 0], "L"))
check_decode("gray_alpha8", Image.fromarray(base[..., :2], "LA"))
check_decode("palette8", Image.fromarray(base[..., :3], "RGB").quantize(256))
check_decode("palette4", Image.fromarray(base[..., :3], "RGB").quantize(16), bits=4)
check_decode("palette2", Image.fromarray(base[..., :3], "RGB").quantize(4), bits=2)
check_decode("palette1", Image.fromarray(base[..., :3], "RGB").quantize(2), bits=1)
check_decode("gray1", Image.fromarray(base[..., 0], "L").convert("1"))
pal_t = Image.fromarray(base[..., :3], "RGB").quantize(64)
pal_t.info["transparency"] = bytes([i * 4 for i in range(64)])
check_decode("palette_trns", pal_t, transparency=pal_t.info["transparency"])
rgba_t = base.copy()
rgba_t[..., 3] = (np.arange(1280) * 255 // 1279).astype(np.uint8)[None, :]
check_decode("rgba8_alpha_ramp", Image.fromarray(rgba_t, "RGBA"))
g16 = (np.mgrid[0:720, 0:1280][1] * 51).astype(np.uint16)
check_decode("gray16", Image.fromarray(g16, "I;16"))
odd = gradient_rgba(333, 77)
check_decode("odd_size_333x77", Image.fromarray(odd, "RGBA"))
check_decode("rgb8_compress0", Image.fromarray(base[..., :3], "RGB"), compress_level=0)

# 16-bit RGBA (PIL can't write it directly): hand-build the PNG.
def write_png(path, w, h, color_type, depth, rows_bytes, extra_chunks=()):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    ihdr = struct.pack(">IIBBBBB", w, h, depth, color_type, 0, 0, 0)
    data = b"".join(b"\x00" + r for r in rows_bytes)
    comp = zlib.compress(data, 9)
    # Split IDAT into several chunks to exercise chunk crossing.
    idats = [chunk(b"IDAT", comp[i:i + 1000]) for i in range(0, len(comp), 1000)]
    body = chunk(b"IHDR", ihdr) + b"".join(extra_chunks) + b"".join(idats) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + body)

w16, h16 = 200, 100
rgba16 = rng.integers(0, 65536, (h16, w16, 4), dtype=np.uint16)
path = os.path.join(WORK, "rgba16_multi_idat.png")
write_png(path, w16, h16, 6, 16, [rgba16[y].astype(">u2").tobytes() for y in range(h16)])
p, raw, scr = run(path)
got = np.fromfile(raw, np.uint8).reshape(h16, w16, 4) if p.returncode == 0 else None
exp = (rgba16 >> 8).astype(np.uint8)
ok = got is not None and np.array_equal(got, exp)
print(f"  {'rgba16_multi_idat':28s} RGBA16 (200, 100)  {'OK' if ok else 'FAIL ' + p.stdout.strip()}")
if not ok:
    failures.append("rgba16_multi_idat")

print("blit tests:")
# Small image: centered 1:1, background elsewhere.
small = np.zeros((100, 200, 4), np.uint8); small[...] = (255, 255, 255, 255)
path = os.path.join(WORK, "small_white.png"); Image.fromarray(small, "RGBA").save(path)
p, raw, scr = run(path, "FF0000")
s = screen(scr)
inside = s[310:410, 540:740]; outside = s[0:300, :]
ok = (inside == (15, 15, 15, 15)).all() and (outside == (15, 0, 0, 15)).all() and (s[309, 540] == (15, 0, 0, 15)).all()
print(f"  small centered + bg           {'OK' if ok else 'FAIL'}")
if not ok: failures.append("small centered")
save_preview(scr, "small_white")

# 1920x1080 -> scaled to 1280x720 exactly.
big = gradient_rgba(1920, 1080)
path = os.path.join(WORK, "big_1080p.png"); Image.fromarray(big, "RGBA").save(path)
p, raw, scr = run(path)
s = screen(scr)
ref = np.asarray(Image.fromarray(big, "RGBA").resize((1280, 720), Image.BOX)).astype(int)
err = np.abs(s[..., :3].astype(int) * 17 - ref[..., :3]).mean()
ok = p.returncode == 0 and err < 12 and (s[..., 3] == 15).all()
print(f"  1920x1080 downscale           {'OK' if ok else 'FAIL'} (mean abs err vs PIL box {err:.1f}/255)")
if not ok: failures.append(f"1080p downscale err {err}")
save_preview(scr, "big_1080p")

# Ultra-wide: letterboxed top/bottom.
wide = gradient_rgba(2560, 720)
path = os.path.join(WORK, "wide.png"); Image.fromarray(wide, "RGBA").save(path)
p, raw, scr = run(path, "00FF00")
s = screen(scr)
ok = (s[0:179] == (0, 15, 0, 15)).all() and (s[541:] == (0, 15, 0, 15)).all() and p.returncode == 0
print(f"  2560x720 letterbox            {'OK' if ok else 'FAIL'}")
if not ok: failures.append("letterbox")

# Transparent image: composited over background, output fully opaque.
tr = np.zeros((720, 1280, 4), np.uint8)
path = os.path.join(WORK, "transparent.png"); Image.fromarray(tr, "RGBA").save(path)
p, raw, scr = run(path, "0000FF")
s = screen(scr)
ok = (s == (0, 0, 15, 15)).all()
print(f"  transparent -> bg             {'OK' if ok else 'FAIL'}")
if not ok: failures.append("transparent over bg")

# Dither preserves average brightness on a flat mid-grey.
grey = np.full((720, 1280, 4), (100, 100, 100, 255), np.uint8)
path = os.path.join(WORK, "grey100.png"); Image.fromarray(grey, "RGBA").save(path)
p, raw, scr = run(path)
mean = screen(scr)[..., 0].mean() * 17
ok = abs(mean - 100) < 3
print(f"  dither mean (100 expected)    {'OK' if ok else 'FAIL'} ({mean:.1f})")
if not ok: failures.append(f"dither mean {mean}")

print("error handling:")
def expect_fail(name, data, want_codes):
    if isinstance(want_codes, int):
        want_codes = (want_codes,)
    path = os.path.join(WORK, name)
    with open(path, "wb") as f:
        f.write(data)
    p, _, _ = run(path)
    code = int(p.stdout.split()[2]) if p.stdout.strip() else None
    # A clean error code (no crash) is what matters.
    ok = p.returncode != 0 and code in want_codes and p.returncode < 128
    print(f"  {name:28s} {'OK' if ok else 'FAIL'} ({p.stdout.strip()})")
    if not ok: failures.append(f"{name}: {p.stdout.strip()} rc={p.returncode}")

good = open(os.path.join(WORK, "rgb8.png"), "rb").read()
expect_fail("not_png.png", b"hello world, not a png", -2)
expect_fail("truncated.png", good[: len(good) // 2], -8)
corrupt = bytearray(good); mid = len(corrupt) // 2
corrupt[mid:mid + 64] = bytes(64)
# Garbage inside the zlib stream surfaces either as an inflate error or as an invalid
# row filter byte, depending on where it lands.
expect_fail("corrupt.png", bytes(corrupt), (-7, -3))
# Interlaced: hand-flip the IHDR interlace byte (offset 8+8+12).
inter = bytearray(good); inter[8 + 8 + 12] = 1
expect_fail("interlaced.png", bytes(inter), -4)
huge = bytearray(good); huge[16:20] = struct.pack(">I", 5000)
expect_fail("too_wide.png", bytes(huge), -5)
expect_fail("empty.png", b"", -2)

print()
if failures:
    print("FAILURES:")
    for f in failures:
        print("  " + f)
    sys.exit(1)
print("ALL TESTS PASSED")
