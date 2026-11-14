"""PC tests for the streaming WAV reader (wav_stream.c).

Usage: python run_wav_tests.py <work_dir>
"""
import os
import struct
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "..", "source"))
WORK = sys.argv[1]
os.makedirs(WORK, exist_ok=True)
EXE = os.path.join(WORK, "test_wav.exe")

subprocess.run(
    ["gcc", "-O2", "-Wall", "-Wextra", "-I", SRC,
     os.path.join(HERE, "test_wav.c"), os.path.join(SRC, "wav_stream.c"), "-o", EXE],
    check=True,
)

rng = np.random.default_rng(7)
failures = []


def wav_bytes(samples, rate, channels, bits=16, fmt_tag=1, extensible=False, extra_chunks=b"", data_len_override=None):
    data = samples.astype("<i2").tobytes() if bits == 16 else samples.astype(np.uint8).tobytes()
    block = channels * bits // 8
    if extensible:
        fmt = struct.pack("<HHIIHHHHI", 0xFFFE, channels, rate, rate * block, block, bits, 22, bits, 3)
        fmt += struct.pack("<H", fmt_tag) + b"\x00\x00\x00\x00\x10\x00\x80\x00\x00\xaa\x00\x38\x9b\x71"
    else:
        fmt = struct.pack("<HHIIHH", fmt_tag, channels, rate, rate * block, block, bits)
    dlen = len(data) if data_len_override is None else data_len_override
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + extra_chunks + b"data" + struct.pack("<I", dlen) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def reference(samples, rate, channels):
    """Same fixed-point linear interpolation as wav_stream.c."""
    x = samples.reshape(-1, channels).astype(np.int64)
    if channels == 1:
        x = np.repeat(x, 2, axis=1)
    n = len(x)
    if n == 0:
        return np.zeros((0, 2), np.int16)
    step = (rate << 32) // 48000
    out = []
    pos = 0
    while (pos >> 32) <= n - 1:
        i = pos >> 32
        f = (pos & 0xFFFFFFFF) >> 16
        a = x[i]
        b = x[i + 1] if i + 1 < n else x[i]
        out.append(a + (((b - a) * f) >> 16))
        pos += step
    return np.array(out, np.int64).astype(np.int16)


def run(name, data, chunk=777):
    path = os.path.join(WORK, name)
    with open(path, "wb") as f:
        f.write(data)
    raw = path + ".raw"
    p = subprocess.run([EXE, path, raw, str(chunk)], capture_output=True, text=True)
    return p, raw


def check(name, samples, rate, channels, chunk=777, **kw):
    p, raw = run(name, wav_bytes(samples, rate, channels, **kw), chunk)
    if p.returncode != 0 or p.stderr:
        failures.append(f"{name}: rc={p.returncode} {p.stdout.strip()} {p.stderr.strip()}")
        print(f"  {name:34s} FAIL {p.stdout.strip()}")
        return
    got = np.fromfile(raw, "<i2").reshape(-1, 2)
    ref = reference(samples, rate, channels)
    ok = got.shape == ref.shape and np.array_equal(got, ref)
    fields = p.stdout.split()
    exp_ms = (len(samples) // channels) * 1000 // rate
    ok = ok and int(fields[3]) == exp_ms
    print(f"  {name:34s} {'OK' if ok else 'FAIL'}  in={len(samples)//channels} out={len(got)} ref={len(ref)} len_ms={fields[3]}")
    if not ok:
        failures.append(name)


print("decode tests:")
t = np.arange(48000)
tone = (np.sin(2 * np.pi * 440 * t / 48000) * 20000).astype(np.int16)
check("48k_stereo_passthrough.wav", np.stack([tone, -tone], -1).ravel(), 48000, 2)
check("48k_mono.wav", tone, 48000, 1)
noise = rng.integers(-32768, 32767, 44100 * 2, dtype=np.int16)
check("44k1_stereo_noise.wav", noise, 44100, 2)
check("22050_mono.wav", rng.integers(-32768, 32767, 22050, dtype=np.int16), 22050, 1)
check("96k_stereo_downsample.wav", rng.integers(-32768, 32767, 96000, dtype=np.int16), 96000, 2)
check("extremes.wav", np.array([32767, -32768] * 500, np.int16), 44100, 1)
check("extensible.wav", noise[:20000], 44100, 2, extensible=True)
check("odd_list_chunk.wav", noise[:20000], 44100, 2, extra_chunks=b"LIST" + struct.pack("<I", 5) + b"abcde\x00")
check("one_frame.wav", np.array([1234, -1234], np.int16), 44100, 2)
check("chunk_1.wav", noise[:5000], 44100, 2, chunk=1)
check("chunk_4096.wav", noise[:50000], 48000, 2, chunk=4096)

print("error handling:")
def expect_fail(name, data, code):
    p, _ = run(name, data)
    got = int(p.stdout.split()[5]) if p.stdout.strip() else None
    ok = p.returncode == 1 and got == code
    print(f"  {name:34s} {'OK' if ok else 'FAIL'} ({p.stdout.strip()})")
    if not ok:
        failures.append(name)

expect_fail("not_wav.wav", b"ID3\x03 this is an mp3", -2)
expect_fail("empty.wav", b"", -2)
expect_fail("8bit.wav", wav_bytes(np.full(100, 128), 44100, 1, bits=8), -4)
expect_fail("float.wav", wav_bytes(tone[:100], 44100, 1, fmt_tag=3), -4)
expect_fail("6ch.wav", wav_bytes(np.zeros(600, np.int16), 48000, 6), -4)
expect_fail("no_data.wav", b"RIFF\x10\x00\x00\x00WAVEfmt \x10\x00\x00\x00" + struct.pack("<HHIIHH", 1, 2, 48000, 192000, 4, 16), -3)

# Data chunk claims more than the file holds: plays what's there, no crash.
p, raw = run("lying_size.wav", wav_bytes(noise[:10000], 44100, 2, data_len_override=10_000_000))
got = np.fromfile(raw, "<i2").reshape(-1, 2) if p.returncode == 0 else None
ok = got is not None and np.array_equal(got, reference(noise[:10000], 44100, 2))
print(f"  {'lying_size.wav':34s} {'OK' if ok else 'FAIL'} ({p.stdout.strip()})")
if not ok:
    failures.append("lying_size")

print()
if failures:
    print("FAILURES:", *failures, sep="\n  ")
    sys.exit(1)
print("ALL TESTS PASSED")
