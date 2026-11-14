"""Converts a Wii U title's boot splash (from its extracted meta/ folder) to SwitchSplash files.

    python tools/wiiu_boot_to_splash.py <wiiu meta dir> <out dir> [--max-seconds N]

Reads:
  bootTvTex.tga     1280x720 TV boot image (TGA, usually uncompressed 24/32-bit)
  bootSound.btsnd   boot jingle: 8-byte big-endian header (u32 format, u32 loop start),
                    then big-endian signed 16-bit stereo PCM at 48 kHz
Writes:
  <out dir>/splash.png
  <out dir>/splash.wav   (48 kHz stereo s16le, trailing silence trimmed)

Prints the jingle's length so you can pick duration_ms (or use duration_ms=0).
Use only with games you own; keep the output out of public repos.
"""
import argparse
import os
import struct
import sys
import wave

import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("meta_dir")
ap.add_argument("out_dir")
ap.add_argument("--max-seconds", type=float, default=None, help="cut the jingle after N seconds (with a short fade)")
ap.add_argument("--silence-db", type=float, default=-60.0, help="level below which the tail counts as silence")
args = ap.parse_args()

os.makedirs(args.out_dir, exist_ok=True)

# --- Image ---
tga = os.path.join(args.meta_dir, "bootTvTex.tga")
img = Image.open(tga)
img.load()
print(f"image: {tga} {img.size} mode={img.mode}")
if img.size != (1280, 720):
    print("  note: not 1280x720; SwitchSplash will scale/center it")
img.convert("RGB").save(os.path.join(args.out_dir, "splash.png"), optimize=True)

# --- Sound ---
snd = os.path.join(args.meta_dir, "bootSound.btsnd")
raw = open(snd, "rb").read()
fmt, loop_start = struct.unpack(">II", raw[:8])
body = raw[8:]
body = body[: len(body) - len(body) % 4]
pcm = np.frombuffer(body, ">i2").reshape(-1, 2).astype(np.int16)
rate = 48000
print(f"sound: {snd} header format={fmt} loop_start={loop_start} frames={len(pcm)} ({len(pcm) / rate:.2f} s)")

# Trim trailing silence (boot sounds are often padded).
level = np.abs(pcm.astype(np.int32)).max(axis=1)
thresh = 32768 * 10 ** (args.silence_db / 20)
loud = np.nonzero(level > thresh)[0]
end = int(loud[-1]) + 1 if len(loud) else len(pcm)
end = min(len(pcm), end + rate // 20)  # keep 50 ms of tail
if args.max_seconds is not None and end > int(args.max_seconds * rate):
    end = int(args.max_seconds * rate)
    fade = min(end, rate // 4)
    pcm = pcm[:end].copy()
    pcm[-fade:] = (pcm[-fade:].astype(np.float32) * np.linspace(1, 0, fade)[:, None]).astype(np.int16)
pcm = pcm[:end]
print(f"  trimmed to {len(pcm) / rate:.2f} s")

with wave.open(os.path.join(args.out_dir, "splash.wav"), "wb") as w:
    w.setnchannels(2)
    w.setsampwidth(2)
    w.setframerate(rate)
    w.writeframes(pcm.astype("<i2").tobytes())

peak = int(np.abs(pcm.astype(np.int32)).max()) if len(pcm) else 0
print(f"  peak {20 * np.log10(max(peak, 1) / 32768):.1f} dBFS")
print(f"wrote {args.out_dir}/splash.png and splash.wav")
