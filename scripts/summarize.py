#!/usr/bin/env python3
"""Aggregate latency samples from a benchmark run into honest percentiles.

Two things this does that a single run cannot:

* Drops the first N samples of every repetition. A consumer that has just attached
  is still faulting in its own page-table entries for the shared-memory ring, one
  fault per 4 KB page for a whole lap. Those faults are startup cost, not transport
  latency, and they land squarely in the tail.

* Reports each percentile as a median across repetitions, with the observed spread.
  One run cannot establish a p99.9 and certainly not a p99.99 -- a single stall
  moves it by orders of magnitude. If the spread across repetitions is wide, the
  number is not yet a measurement, and printing it next to the median says so.

Usage:
    summarize.py [--drop N] [--json] DIR [DIR ...]

Each DIR is a directory of rep<R>_c<I>.csv files as written by bench.sh.
"""

import argparse
import csv
import json
import re
import statistics
import sys
from pathlib import Path

PERCENTILES = [50.0, 99.0, 99.9, 99.99]
REP_RE = re.compile(r"rep(\d+)_c(\d+)\.csv$")


def load(path, drop):
    """Return steady-state latency samples from one CSV, or None if too short."""
    out = []
    with open(path, newline="") as handle:
        reader = csv.reader(handle)
        next(reader, None)  # header: seq,latency_ns
        for row in reader:
            if len(row) >= 2:
                out.append(int(row[1]))
    if len(out) <= drop:
        return None
    return out[drop:]


def pct(sorted_vals, p):
    """Nearest-rank percentile, matching the harness's own definition so the two
    sets of numbers can be compared directly."""
    n = len(sorted_vals)
    rank = int(p / 100.0 * n)
    if rank < p / 100.0 * n:
        rank += 1
    rank = max(1, min(rank, n))
    return sorted_vals[rank - 1]


def summarize_dir(directory, drop):
    files = sorted(Path(directory).glob("rep*_c*.csv"))
    if not files:
        return None

    # Percentiles are computed per (repetition, consumer) and then aggregated, so a
    # slow consumer or a bad repetition is visible rather than averaged away.
    per_run = []
    for path in files:
        match = REP_RE.search(path.name)
        if not match:
            continue
        samples = load(path, drop)
        if samples is None:
            print(f"  skipping {path.name}: fewer than {drop} samples", file=sys.stderr)
            continue
        samples.sort()
        per_run.append(
            {
                "rep": int(match.group(1)),
                "consumer": int(match.group(2)),
                "n": len(samples),
                "min": samples[0],
                "max": samples[-1],
                **{f"p{p:g}": pct(samples, p) for p in PERCENTILES},
            }
        )
    if not per_run:
        return None

    agg = {"name": Path(directory).name, "runs": len(per_run),
           "reps": len({r["rep"] for r in per_run}),
           "consumers": len({r["consumer"] for r in per_run}),
           "samples_per_run": min(r["n"] for r in per_run)}
    for key in ["min"] + [f"p{p:g}" for p in PERCENTILES] + ["max"]:
        vals = [r[key] for r in per_run]
        agg[key] = {
            "median": int(statistics.median(vals)),
            "lo": min(vals),
            "hi": max(vals),
        }
    agg["_per_run"] = per_run
    return agg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--drop", type=int, default=16384,
                    help="samples to discard at the start of each run")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    results = []
    for directory in args.dirs:
        agg = summarize_dir(directory, args.drop)
        if agg is None:
            print(f"no usable samples in {directory}", file=sys.stderr)
            continue
        results.append(agg)

    if args.json:
        for r in results:
            r.pop("_per_run", None)
        print(json.dumps(results, indent=2))
        return

    if not results:
        sys.exit(1)

    head = f"{'config':<22}{'reps':>5}{'cons':>5}" + "".join(
        f"{k:>12}" for k in ["min", "p50", "p99", "p99.9", "p99.99", "max"])
    print(head)
    print("-" * len(head))
    for r in results:
        row = f"{r['name']:<22}{r['reps']:>5}{r['consumers']:>5}"
        for key in ["min", "p50", "p99", "p99.9", "p99.99", "max"]:
            row += f"{r[key]['median']:>12,}"
        print(row)
        # The spread across repetitions is the honesty column: a p99.99 whose
        # median and extremes differ by an order of magnitude is not established.
        spread = f"{'  spread lo..hi':<22}{'':>5}{'':>5}"
        for key in ["min", "p50", "p99", "p99.9", "p99.99", "max"]:
            spread += f"{r[key]['lo']:>5,}..{r[key]['hi']:<6,}"
        print(spread)


if __name__ == "__main__":
    main()
