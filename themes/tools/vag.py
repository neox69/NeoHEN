#!/usr/bin/env python3
"""Turn a WAV file into a menu sound for a PS5 .rco bundle.

  python3 themes/tools/vag.py <in.wav> <out.vag> [name]

The menu's sounds are HEVAG files: a 48-byte "VAGp" header (version
0x00020001) and 16-byte ADPCM frames of 28 samples, the two channels taking
turns frame by frame. HEVAG has 128 prediction filters; its first five are
the classic PlayStation ones, and those are the ones used here.

Input: 16-bit PCM WAV, mono or stereo. It is resampled to 48 kHz if needed
(linear, fine for short effects). Give the result to rco.py replace.
"""
import struct
import sys
import wave

RATE = 48000
FILTERS = ((0, 0), (60, 0), (115, -52), (98, -55), (122, -60))  # in 64ths


def clamp16(v):
    return -32768 if v < -32768 else 32767 if v > 32767 else v


def encode_frame(samples, h1, h2):
    """The best (filter, shift) for 28 samples. Returns the 16 bytes without
    the flag, and the decoder's last two samples."""
    best = None
    for f, (c1, c2) in enumerate(FILTERS):
        # how big the residue gets with the true samples picks the shift
        a, b, peak = h1, h2, 0
        for x in samples:
            d = abs(x - ((a * c1 + b * c2) >> 6))
            if d > peak:
                peak = d
            a, b = x, a
        shift = 12
        while shift > 0 and (7 << (12 - shift)) < peak:
            shift -= 1
        for s in {max(0, shift - 1), shift, min(12, shift + 1)}:
            step = 12 - s
            a, b, err, nibs = h1, h2, 0, []
            for x in samples:
                pred = (a * c1 + b * c2) >> 6
                n = (x - pred + ((1 << step) >> 1)) >> step
                n = -8 if n < -8 else 7 if n > 7 else n
                y = clamp16(pred + (n << step))
                err += (x - y) * (x - y)
                nibs.append(n & 15)
                a, b = y, a
            if best is None or err < best[0]:
                best = (err, f, s, nibs, a, b)
    _, f, s, nibs, a, b = best
    body = bytes(nibs[i] | (nibs[i + 1] << 4) for i in range(0, 28, 2))
    return (f << 4) | s, body, a, b


def encode(channels, rate=RATE, name="sound"):
    """channels: one or two lists of 16-bit samples. Returns the .vag bytes."""
    if len(channels) == 1:
        channels = [channels[0], channels[0]]
    count = max(len(c) for c in channels)
    count += -count % 28
    channels = [list(c) + [0] * (count - len(c)) for c in channels]
    hist = [(0, 0), (0, 0)]
    out = bytearray(32)  # one silent frame per channel, as the console's have
    frames = count // 28
    for i in range(frames):
        for ch in range(2):
            head, body, a, b = encode_frame(channels[ch][i * 28:(i + 1) * 28], *hist[ch])
            hist[ch] = (a, b)
            out += bytes((head, 1 if i == frames - 1 else 0)) + body
    out += (b"\x00\x07" + b"\x77" * 14) * 2
    header = struct.pack(">4sIIII10xBx16s", b"VAGp", 0x00020001, 0, len(out), rate, 2,
                         name.encode("ascii", "replace")[:15])
    return header + bytes(out)


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2 or w.getnchannels() not in (1, 2):
            raise SystemExit("16-bit PCM WAV, mono or stereo, is what is taken")
        n, rate, raw = w.getnchannels(), w.getframerate(), w.readframes(w.getnframes())
    flat = struct.unpack("<%dh" % (len(raw) // 2), raw)
    channels = [list(flat[c::n]) for c in range(n)]
    if rate != RATE:
        length = int(len(channels[0]) * RATE / rate)
        resampled = []
        for c in channels:
            c = c + [0]
            new = []
            for i in range(length):
                pos = i * rate / RATE
                k = int(pos)
                frac = pos - k
                new.append(int(c[k] * (1 - frac) + c[min(k + 1, len(c) - 1)] * frac))
            resampled.append(new)
        channels = resampled
    return channels


def main():
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    channels = read_wav(sys.argv[1])
    data = encode(channels, name=sys.argv[3] if len(sys.argv) == 4 else "neohen")
    with open(sys.argv[2], "wb") as f:
        f.write(data)
    print(f"{len(channels[0]) / RATE:.2f} s -> {sys.argv[2]} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
