#!/usr/bin/env python3
"""Latency and loss against message rate, one line per arm.

Reads the blocks.csv that ab_bench.sh writes, one directory per rate. Every point is
the median across blocks of that arm at that rate, and the band is the full spread of
the blocks behind it -- the same quantity paired_stats.py calls the between-block
spread, drawn rather than quoted, because a percentile plotted without it invites a
reader to believe differences the design cannot resolve.
"""
import csv
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

PCTS = [("e2e_p50", "p50"), ("e2e_p99", "p99"),
        ("e2e_p999", "p99.9"), ("e2e_p9999", "p99.99")]


def load(dirs):
    rows = defaultdict(lambda: defaultdict(list))
    loss = defaultdict(lambda: defaultdict(list))
    for d in dirs:
        rate = int(Path(d).name.rsplit("_", 1)[1])
        with open(Path(d) / "blocks.csv") as fh:
            for r in csv.DictReader(fh):
                if r["valid"] != "1":
                    continue
                for key, _ in PCTS:
                    if r[key]:
                        rows[(r["arm"], key)][rate].append(float(r[key]))
                if r["loss_pct"]:
                    loss[r["arm"]][rate].append(float(r["loss_pct"]))
    return rows, loss


def med(v):
    v = sorted(v)
    n = len(v)
    return v[n // 2] if n % 2 else 0.5 * (v[n // 2 - 1] + v[n // 2])


def main(dirs, out):
    rows, loss = load(dirs)
    arms = sorted({a for a, _ in rows}, reverse=True)
    style = {arms[0]: dict(ls="-", marker="o"), arms[-1]: dict(ls="--", marker="s")}
    fig, axes = plt.subplots(1, 5, figsize=(21, 4.2))

    for ax, (key, label) in zip(axes, PCTS):
        for arm in arms:
            rates = sorted(rows[(arm, key)])
            if not rates:
                continue
            m = [med(rows[(arm, key)][r]) / 1000.0 for r in rates]
            lo = [min(rows[(arm, key)][r]) / 1000.0 for r in rates]
            hi = [max(rows[(arm, key)][r]) / 1000.0 for r in rates]
            ax.fill_between(rates, lo, hi, alpha=0.15)
            ax.plot(rates, m, label=arm, **style[arm])
        ax.set(xscale="log", yscale="log", title=f"end-to-end {label}",
               xlabel="message rate (msg/s)")
        ax.grid(True, which="both", alpha=0.25)
    axes[0].set_ylabel("latency (us)")
    axes[0].legend()

    ax = axes[4]
    for arm in arms:
        rates = sorted(loss[arm])
        m = [med(loss[arm][r]) for r in rates]
        lo = [min(loss[arm][r]) for r in rates]
        hi = [max(loss[arm][r]) for r in rates]
        ax.fill_between(rates, lo, hi, alpha=0.15)
        ax.plot(rates, m, label=arm, **style[arm])
    ax.set(xscale="log", title="frames never delivered",
           xlabel="message rate (msg/s)", ylabel="loss (%)")
    ax.grid(True, which="both", alpha=0.25)
    ax.legend()

    fig.tight_layout()
    fig.savefig(out, dpi=140)
    print(out)


if __name__ == "__main__":
    main(sys.argv[1:-1], sys.argv[-1])
