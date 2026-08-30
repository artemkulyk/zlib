#!/usr/bin/env python3
"""Compare two zlib builds on inflate throughput.

Stock and optimized runs are interleaved and repeated in rounds, with the
order swapped each round, so a runner that speeds up or slows down partway
through cannot masquerade as a difference between the builds.  A delta is
only called real when every round agrees on its direction.
"""

import argparse
import os
import re
import statistics
import subprocess
import sys

LINE = re.compile(r"median=([0-9.eE+-]+).*?mbs=([0-9.eE+-]+)")

# A verdict needs this much median difference before it is worth reporting.
THRESHOLD = 0.03


def parse_modes(spec):
    modes = []
    for item in spec.split(","):
        if item.startswith("chunk:"):
            modes.append(("chunk", int(item.split(":", 1)[1])))
        else:
            modes.append((item, None))
    return modes


def mode_name(mode, chunk):
    return "%s %d" % (mode, chunk) if chunk else mode


def measure(binary, data, corpus, mode, chunk, iters, warmup, clock):
    src = os.path.join(data, corpus + (".raw" if mode == "back" else ".gz"))
    out_bytes = str(os.path.getsize(os.path.join(data, corpus + ".bin")))
    cmd = [binary, "--mode", mode, "--iters", str(iters),
           "--warmup", str(warmup), "--clock", clock]
    if chunk:
        cmd += ["--chunk", str(chunk)]
    cmd += [src, out_bytes]
    proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True)
    if proc.returncode != 0:
        sys.stderr.write("failed: %s\n%s%s\n" % (" ".join(cmd), proc.stdout,
                                                 proc.stderr))
        raise SystemExit(1)
    m = LINE.search(proc.stdout)
    if not m:
        sys.stderr.write("unparsed output: %s\n" % proc.stdout)
        raise SystemExit(1)
    return float(m.group(1)), float(m.group(2))


def verdict(ratios):
    """Ratio is stock_time / opt_time, so above 1 means the change is faster."""
    med = statistics.median(ratios)
    if abs(med - 1.0) < THRESHOLD:
        return "flat"
    if med > 1.0:
        return "faster" if all(r > 1.0 for r in ratios) else "unstable"
    return "SLOWER" if all(r < 1.0 for r in ratios) else "unstable"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stock", required=True)
    ap.add_argument("--opt", required=True)
    ap.add_argument("--data", default="corpora")
    ap.add_argument("--csv", default="bench.csv")
    ap.add_argument("--label", default="local")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--iters", type=int, default=25)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--clock", default="cpu", choices=["cpu", "mono"])
    ap.add_argument("--corpora", default="text,words,random,pngish,zeros,"
                                        "rep2,rep16,maxmatch")
    ap.add_argument("--modes", default="full,chunk:258,chunk:16384,back")
    args = ap.parse_args()

    corpora = args.corpora.split(",")
    modes = parse_modes(args.modes)
    rows = []
    summary = []

    for corpus in corpora:
        for mode, chunk in modes:
            ratios, stock_mbs, opt_mbs = [], [], []
            for rnd in range(args.rounds):
                builds = [("stock", args.stock), ("opt", args.opt)]
                if rnd % 2:
                    builds.reverse()
                seen = {}
                for build, binary in builds:
                    med, mbs = measure(binary, args.data, corpus, mode, chunk,
                                       args.iters, args.warmup, args.clock)
                    seen[build] = (med, mbs)
                    rows.append([args.label, corpus, mode_name(mode, chunk),
                                 str(rnd), build, "%.9f" % med, "%.2f" % mbs])
                ratios.append(seen["stock"][0] / seen["opt"][0])
                stock_mbs.append(seen["stock"][1])
                opt_mbs.append(seen["opt"][1])
            name = mode_name(mode, chunk)
            call = verdict(ratios)
            summary.append((corpus, name, statistics.median(stock_mbs),
                            statistics.median(opt_mbs),
                            statistics.median(ratios), min(ratios),
                            max(ratios), call))
            print("%-9s %-12s stock=%9.1f opt=%9.1f MB/s  ratio=%.3f "
                  "[%.3f..%.3f]  %s" %
                  (corpus, name, summary[-1][2], summary[-1][3],
                   summary[-1][4], min(ratios), max(ratios), call), flush=True)

    with open(args.csv, "w") as f:
        f.write("label,corpus,mode,round,build,median_s,mbs\n")
        for row in rows:
            f.write(",".join(row) + "\n")

    table = ["| corpus | mode | stock MB/s | opt MB/s | ratio | spread | verdict |",
             "|---|---|---:|---:|---:|---|---|"]
    for corpus, name, s, o, med, lo, hi, call in summary:
        table.append("| %s | %s | %.1f | %.1f | **%.3f** | %.3f..%.3f | %s |" %
                     (corpus, name, s, o, med, lo, hi, call))
    regressions = [s for s in summary if s[7] == "SLOWER"]
    body = "\n".join([
        "### %s" % args.label,
        "",
        "Ratio is stock time over opt time, so above 1.000 means the change is "
        "faster. `%s` clock, %d rounds of %d iterations, builds interleaved."
        % (args.clock, args.rounds, args.iters),
        "",
        "\n".join(table),
        "",
        ("**%d slower case(s) consistent across all rounds.**" % len(regressions)
         if regressions else "No consistent slowdown."),
        "",
    ])
    step = os.environ.get("GITHUB_STEP_SUMMARY")
    if step:
        with open(step, "a") as f:
            f.write(body + "\n")
    print()
    print(body)


if __name__ == "__main__":
    main()
