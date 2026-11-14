"""Generates test jingles for hardware testing.

    python tools/make_test_jingle.py <out_dir>

Writes:
  jingle_48k_stereo.wav    ~3.5 s chime, 48 kHz stereo (played exactly as-is)
  jingle_44k1_mono.wav     same chime, 44.1 kHz mono (exercises resampling + mono->stereo)
  tone_10s_48k.wav         10 s steady chord: with duration_ms=6000 you should hear it
                           fade out over audio_fade_ms when the image disappears.
"""
import os
import sys
import wave

import numpy as np


def chime(rate):
    t = np.arange(int(rate * 3.5)) / rate
    out = np.zeros_like(t)
    # Rising arpeggio C5 E5 G5 C6, each note a decaying bell.
    for i, f in enumerate([523.25, 659.25, 783.99, 1046.5]):
        start = i * 0.35
        tt = np.clip(t - start, 0, None)
        env = np.where(t >= start, np.exp(-tt * 2.2), 0.0)
        out += env * (np.sin(2 * np.pi * f * tt) + 0.3 * np.sin(2 * np.pi * 2 * f * tt))
    out *= 0.25
    # Short fade-in to avoid a click.
    out[: int(rate * 0.005)] *= np.linspace(0, 1, int(rate * 0.005))
    return out


def chord(rate, seconds):
    t = np.arange(int(rate * seconds)) / rate
    out = sum(np.sin(2 * np.pi * f * t) for f in (261.63, 329.63, 392.0)) * 0.15
    ramp = int(rate * 0.01)
    out[:ramp] *= np.linspace(0, 1, ramp)
    return out


def write(path, samples, rate, channels):
    data = (np.clip(samples, -1, 1) * 32767).astype("<i2")
    if channels == 2 and data.ndim == 1:
        # Slight stereo spread so a mono/stereo mixup is audible.
        data = np.stack([data, (data * 0.8).astype("<i2")], -1)
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data.tobytes())
    print(f"wrote {path} ({os.path.getsize(path) // 1024} KiB)")


out = sys.argv[1] if len(sys.argv) > 1 else "samples"
os.makedirs(out, exist_ok=True)
write(os.path.join(out, "jingle_48k_stereo.wav"), chime(48000), 48000, 2)
write(os.path.join(out, "jingle_44k1_mono.wav"), chime(44100), 44100, 1)
write(os.path.join(out, "tone_10s_48k.wav"), chord(48000, 10), 48000, 2)
