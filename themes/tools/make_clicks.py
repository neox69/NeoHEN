#!/usr/bin/env python3
"""Make a small set of menu sounds from nothing: dry, mechanical clicks.

  python3 themes/tools/make_clicks.py <out dir> [--gain 1.0]

Writes snd_focus_move, snd_enter, snd_cancel, snd_open_option_menu and
snd_close_option_menu as .wav (to listen to) and .vag (for rco.py replace).
Everything is synthesised here, so the result can be shared freely.

Needs numpy.
"""
import os
import struct
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vag  # noqa: E402

RATE = 48000


def t(seconds):
    return np.arange(int(RATE * seconds)) / RATE


def ping(freq, decay, seconds, phase=0.0):
    x = t(seconds)
    return np.sin(2 * np.pi * freq * x + phase) * np.exp(-x / decay)


def tick(seconds, colour, seed):
    """A burst of noise, shaped: the 'contact' of a switch."""
    rng = np.random.default_rng(seed)
    n = rng.standard_normal(int(RATE * seconds))
    # one-pole filters: colour 0 = dull, 1 = bright
    low = np.empty_like(n)
    acc = 0.0
    k = 0.05 + 0.6 * colour
    for i, v in enumerate(n):
        acc += k * (v - acc)
        low[i] = acc
    x = t(seconds)
    return (low - np.mean(low)) * np.exp(-x / (seconds / 5))


def place(length, *parts):
    out = np.zeros(int(RATE * length))
    for start, gain, sig in parts:
        a = int(RATE * start)
        b = min(len(out), a + len(sig))
        out[a:b] += gain * sig[:b - a]
    return out


def room(sig, amount=0.12):
    """A few short echoes, so it is not bone dry; different left and right."""
    left, right = sig.copy(), sig.copy()
    for ms, g in ((11, 0.5), (23, 0.3), (37, 0.18)):
        d = int(RATE * ms / 1000)
        left[d:] += amount * g * sig[:-d]
    for ms, g in ((13, 0.5), (29, 0.3), (41, 0.18)):
        d = int(RATE * ms / 1000)
        right[d:] += amount * g * sig[:-d]
    return left, right


def sounds():
    return {
        # the one heard most: tiny, dry, a hint of metal
        "snd_focus_move": (0.20, place(
            0.09,
            (0.000, 0.9, tick(0.006, 0.9, 1)),
            (0.001, 0.5, ping(2350, 0.006, 0.05)),
            (0.001, 0.35, ping(1180, 0.010, 0.06)))),
        # a switch going home: click, then a low body
        "snd_enter": (0.30, place(
            0.22,
            (0.000, 1.0, tick(0.008, 0.8, 2)),
            (0.002, 0.7, ping(640, 0.030, 0.18)),
            (0.002, 0.45, ping(1620, 0.012, 0.08)),
            (0.034, 0.5, tick(0.005, 0.5, 3)),
            (0.036, 0.35, ping(320, 0.040, 0.16)))),
        # the same switch coming back: lower, duller
        "snd_cancel": (0.28, place(
            0.20,
            (0.000, 0.9, tick(0.009, 0.4, 4)),
            (0.002, 0.7, ping(410, 0.032, 0.17)),
            (0.002, 0.3, ping(980, 0.012, 0.08)),
            (0.028, 0.35, tick(0.005, 0.3, 5)))),
        "snd_open_option_menu": (0.26, place(
            0.26,
            (0.000, 0.8, tick(0.007, 0.7, 6)),
            (0.002, 0.5, ping(520, 0.035, 0.2)),
            (0.045, 0.8, tick(0.006, 0.9, 7)),
            (0.047, 0.5, ping(780, 0.040, 0.2)))),
        "snd_close_option_menu": (0.26, place(
            0.26,
            (0.000, 0.8, tick(0.007, 0.8, 8)),
            (0.002, 0.5, ping(780, 0.035, 0.2)),
            (0.045, 0.8, tick(0.007, 0.5, 9)),
            (0.047, 0.5, ping(520, 0.040, 0.2)))),
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = sys.argv[1]
    gain = float(sys.argv[sys.argv.index("--gain") + 1]) if "--gain" in sys.argv else 1.0
    os.makedirs(out, exist_ok=True)
    for name, (peak, sig) in sounds().items():
        left, right = room(sig)
        scale = peak * gain / max(np.max(np.abs(left)), np.max(np.abs(right)))
        fade = np.minimum(1.0, np.arange(len(left))[::-1] / (RATE * 0.01))
        chans = [np.clip(c * scale * fade * 32767, -32767, 32767).astype(np.int16) for c in (left, right)]
        with wave.open(os.path.join(out, name + ".wav"), "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(RATE)
            inter = np.empty(len(left) * 2, dtype=np.int16)
            inter[0::2], inter[1::2] = chans
            w.writeframes(struct.pack("<%dh" % len(inter), *inter.tolist()))
        data = vag.encode([c.tolist() for c in chans], name=name[4:])
        with open(os.path.join(out, name + ".vag"), "wb") as f:
            f.write(data)
        print(f"{name}: {len(left) / RATE * 1000:.0f} ms, {len(data)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
