"""Repeated chunks and dropouts of a loopback recording of the game's music.

Usage: python -I analyze.py <recording>...   (.npy from record_menu.py, or a 16/32-bit .wav)

Repeats: the share of 10 ms chunks that repeat a chunk from 5 to 200 ms
earlier (correlation above 0.97), and the dominant lags. A mixer that replays
stale blocks shows a high share at one lag (issue #86: 2400 samples).
Dropouts: windows of 2 ms at least 30 dB under the level around them.
Acceptance of the native audio layer: at most 1% repeats and no dropout.
"""
import struct
import sys

import numpy as np

RATE = 48000
CHUNK = 480
MAX_REPEATS = 1.0


def load(path):
    if path.endswith(".npy"):
        x = np.load(path).astype(np.float64)
    else:
        x = read_wav(path)
    return x.mean(axis=1) if x.ndim == 2 else x


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a WAV file")
    at, fmt, channels, bits, samples = 12, None, 0, 0, None
    while at + 8 <= len(data):
        tag, size = data[at:at + 4], struct.unpack("<I", data[at + 4:at + 8])[0]
        body = data[at + 8:at + 8 + size]
        if tag == b"fmt ":
            fmt, channels = struct.unpack("<HH", body[:4])
            bits = struct.unpack("<H", body[14:16])[0]
            if fmt == 0xFFFE:
                fmt = struct.unpack("<H", body[24:26])[0]
        elif tag == b"data":
            samples = body
        at += 8 + size + (size & 1)
    if samples is None:
        raise ValueError(f"{path}: no data chunk")
    if fmt == 3 and bits == 32:
        x = np.frombuffer(samples[:len(samples) // 4 * 4], dtype="<f4").astype(np.float64)
    elif fmt == 1 and bits == 16:
        x = np.frombuffer(samples[:len(samples) // 2 * 2], dtype="<i2").astype(np.float64) / 32768.0
    else:
        raise ValueError(f"{path}: format {fmt} with {bits} bits is not supported")
    return x[:len(x) // channels * channels].reshape(-1, channels)


def repeats(x):
    lags = np.arange(240, 9600, 2)
    farthest = int(lags[-1])
    energy = np.concatenate([[0.0], np.cumsum(x * x)])
    rows = []
    for start in range(CHUNK * 30, len(x) - CHUNK, CHUNK // 2):
        a = x[start:start + CHUNK]
        if np.sqrt((a ** 2).mean()) < 1e-3:
            continue
        # The dot products with the chunk at every lag in one pass; the norms from the running energy.
        dots = np.correlate(x[start - farthest:start - lags[0] + CHUNK], a, "valid")[farthest - lags]
        begins = start - lags
        c = dots / (np.linalg.norm(a) * np.sqrt(np.maximum(energy[begins + CHUNK] - energy[begins], 0)) + 1e-12)
        best = int(np.argmax(c))
        rows.append((float(c[best]), int(lags[best])))
    strong = [lag for c, lag in rows if c > 0.97]
    lags = {}
    for lag in strong:
        lags[lag] = lags.get(lag, 0) + 1
    top = sorted(lags.items(), key=lambda kv: -kv[1])[:5]
    return 100 * len(strong) / max(len(rows), 1), len(rows), [(lag, round(lag / 48, 2), n) for lag, n in top]


def dropouts(x):
    window = 96  # 2 ms
    n = len(x) // window
    rms = np.sqrt((x[:n * window].reshape(n, window) ** 2).mean(axis=1))
    db = 20 * np.log10(rms + 1e-9)
    reference = np.array([np.median(db[max(0, i - 50):i + 50]) for i in range(n)])
    quiet = (db < reference - 30) & (reference > -60)
    runs, i = [], 0
    while i < n:
        if quiet[i]:
            j = i
            while j < n and quiet[j]:
                j += 1
            runs.append((i * 2, (j - i) * 2))
            i = j
        else:
            i += 1
    return runs


def analyze(path):
    x = load(path)
    level = 20 * np.log10(np.sqrt((x ** 2).mean()) + 1e-12)
    share, chunks, top = repeats(x)
    gaps = dropouts(x)
    passed = bool(share <= MAX_REPEATS and not gaps and level > -60)
    return dict(file=path, seconds=round(len(x) / RATE, 1), level_db=round(float(level), 1), chunks=chunks,
                repeats_percent=round(share, 2), top_lags=top, dropouts=len(gaps), first_dropouts=gaps[:8],
                passed=passed)


if __name__ == "__main__":
    failed = False
    for path in sys.argv[1:]:
        r = analyze(path)
        failed |= not r["passed"]
        print(f"{'PASS' if r['passed'] else 'FAIL'} {path}: {r['seconds']} s, level {r['level_db']} dB, "
              f"repeats {r['repeats_percent']}% of {r['chunks']} chunks (top lags {r['top_lags']}), "
              f"dropouts {r['dropouts']} {r['first_dropouts']}")
    sys.exit(1 if failed else 0)
