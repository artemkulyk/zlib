#!/usr/bin/env python3
"""Generate deterministic inflate corpora.

Each corpus is written three ways: raw bytes (.bin), gzip (.gz) for inflate(),
and headerless deflate (.raw) for inflateBack().  Output is identical on every
machine so ratios from different runners are comparable.
"""

import argparse
import gzip
import os
import random
import zlib

DEFAULT_SIZE = 8 * 1024 * 1024


def tile(pattern, n):
    if not pattern:
        raise ValueError("empty pattern")
    reps = (n // len(pattern)) + 1
    return (pattern * reps)[:n]


def source_text(repo, n):
    """zlib's own C sources, tiled.  Short LZ77 matches, like real gzip."""
    skip = (".git", "contrib", "os400", "msdos", "amiga", "watcom", "qnx",
            "nintendods", "old", ".github")
    chunks = []
    for dirpath, dirnames, files in os.walk(repo):
        dirnames[:] = [d for d in dirnames if d not in skip]
        for name in sorted(files):
            if name.endswith((".c", ".h")):
                with open(os.path.join(dirpath, name), "rb") as f:
                    chunks.append(f.read())
    blob = b"\n".join(chunks) or b"zlib inflate benchmark corpus\n"
    return tile(blob, n)


def wordish(n, seed=20260830):
    """Deterministic pseudo-English text; no dependency on a system word list."""
    rng = random.Random(seed)
    letters = "etaoinshrdlucmfwypvbgkjqxz"
    words = []
    for _ in range(4096):
        length = rng.randint(2, 11)
        words.append("".join(rng.choice(letters) for _ in range(length)))
    out = bytearray()
    while len(out) < n:
        line = " ".join(rng.choice(words) for _ in range(rng.randint(6, 14)))
        out += line.encode("ascii") + b"\n"
    return bytes(out[:n])


def pngish(n, width=1024, rows=64):
    """PNG Sub-filtered scanlines: long non-overlapping matches between rows."""
    scanlines = []
    for r in range(rows):
        row = bytearray()
        row.append(1)                           # PNG Sub filter tag
        prev = 0
        for x in range(width):
            val = (x * 7 + r * 31) & 0xFF
            row.append((val - prev) & 0xFF)
            prev = val
        scanlines.append(bytes(row))
    return tile(b"".join(scanlines), n)


def repeating(period, n):
    return tile(bytes((i * 17 + 31) & 0xFF for i in range(period)), n)


def build(name, size, repo):
    if name == "text":
        return source_text(repo, size)
    if name == "words":
        return wordish(size)
    if name == "random":
        return random.Random(1).randbytes(size)
    if name == "pngish":
        return pngish(size)
    if name == "zeros":
        return b"\x00" * size
    if name == "rle_A":
        return b"A" * size
    if name == "maxmatch":
        return repeating(258, size)
    if name.startswith("rep"):
        return repeating(int(name[3:]), size)
    raise SystemExit("unknown corpus: " + name)


def write(outdir, name, raw):
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, name + ".bin"), "wb") as f:
        f.write(raw)
    # mtime=0 keeps the gzip header byte-identical between runs
    with gzip.GzipFile(os.path.join(outdir, name + ".gz"), "wb",
                       compresslevel=6, mtime=0) as f:
        f.write(raw)
    comp = zlib.compressobj(6, zlib.DEFLATED, -15)
    with open(os.path.join(outdir, name + ".raw"), "wb") as f:
        f.write(comp.compress(raw) + comp.flush())
    gz = os.path.getsize(os.path.join(outdir, name + ".gz"))
    print("%-10s raw=%.2f MiB gz=%.1f KiB" %
          (name, len(raw) / 1048576.0, gz / 1024.0))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="corpora")
    ap.add_argument("--size", type=int, default=DEFAULT_SIZE)
    ap.add_argument("--repo", default=os.path.abspath(
        os.path.join(here, "..", "..")))
    ap.add_argument("--corpora", default="text,words,random,pngish,zeros,"
                                         "rle_A,rep2,rep15,rep16,rep32,"
                                         "maxmatch")
    args = ap.parse_args()
    for name in args.corpora.split(","):
        write(args.out, name, build(name, args.size, args.repo))


if __name__ == "__main__":
    main()
