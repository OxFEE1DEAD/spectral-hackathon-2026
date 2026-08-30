#!/usr/bin/env python3
"""Aggregate latency samples from a benchmark run into honest percentiles.

Two things this does that a single run cannot:

* Drops the first N samples of every repetition. A consumer that has just attached
  is still faulting in its own page-table entries for the shared-memory ring, one
  fault per 4 KB page for a whole lap. Those faults are startup cost, not transport
  latency, and they land squarely in the tail.

* Pools samples before taking a percentile, rather than taking the median of
  per-file percentiles. This matters far more than it sounds: on real runs the two
  disagree by up to 719% at p99.99, because the median across files discards
  precisely the files that contain the rare events -- which is where a p99.99
  lives. Measured on four ten-consumer runs, median-of-per-file understated the
  p99.99 by 1.4x, 3.4x, 8.2x and 1.7x. Pooling uses every sample that was
  collected, which is the whole reason for collecting them.

* Reports a spread, and is explicit that it is a *within-session* spread. It is
  the range of per-repetition pooled values, so it captures variation between
  repetitions of one run in one window. It does NOT capture the drift of the two
  hosts' clocks between windows, nor the environment's own between-window mood --
  three runs of an identical configuration have produced p99.99 values spanning
  14.5x. Quoting this column as the uncertainty of a comparison is what caused two
  published results to be withdrawn. For comparisons between configurations use
  ab_bench.sh and paired_stats.py, which measure across blocks instead.

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

    # Samples are kept per repetition and pooled, not reduced to a percentile per file.
    # A percentile of a pool of N*M samples is an estimate from N*M samples; the median of
    # N per-file percentiles is not, and at p99.99 it is wrong by multiples.
    by_rep = {}
    skipped = []
    for path in files:
        match = REP_RE.search(path.name)
        if not match:
            continue
        samples = load(path, drop)
        if samples is None:
            skipped.append(path.name)
            continue
        by_rep.setdefault(int(match.group(1)), []).extend(samples)
    if skipped:
        # Loud, and counted: a repetition that vanished is a fact about the measurement,
        # not a detail. Printing it and carrying on is how a comparison comes to rest on a
        # single surviving repetition whose spread therefore reads as zero.
        print(f"  WARNING: {len(skipped)} file(s) had fewer than {drop} samples and were "
              f"excluded: {', '.join(skipped)}", file=sys.stderr)
    if not by_rep:
        return None

    # Per-repetition pooled percentiles give the within-session spread; everything pooled
    # gives the headline estimate.
    per_rep = {}
    for rep, vals in by_rep.items():
        vals.sort()
        per_rep[rep] = {"n": len(vals), "min": vals[0], "max": vals[-1],
                        **{f"p{p:g}": pct(vals, p) for p in PERCENTILES}}

    pooled = sorted(v for vals in by_rep.values() for v in vals)

    n_consumers = len({int(REP_RE.search(p.name).group(2))
                       for p in files if REP_RE.search(p.name)})
    agg = {"name": Path(directory).name,
           "runs": len(files) - len(skipped),
           "reps": len(by_rep),
           "consumers": n_consumers,
           "excluded_files": len(skipped),
           "samples_per_run": min(r["n"] for r in per_rep.values()),
           "samples_pooled": len(pooled)}
    for key in ["min"] + [f"p{p:g}" for p in PERCENTILES] + ["max"]:
        if key == "min":
            headline = pooled[0]
        elif key == "max":
            headline = pooled[-1]
        else:
            headline = pct(pooled, float(key[1:]))
        vals = [r[key] for r in per_rep.values()]
        agg[key] = {
            "median": int(headline),          # pooled estimate, not a median of medians
            "lo": min(vals),                  # within-session spread across repetitions
            "hi": max(vals),
        }
    # A percentile needs roughly 10/(1-p) samples to mean anything; below that it is the
    # single worst observation wearing a percentile's name.
    agg["_resolvable"] = {f"p{p:g}": len(pooled) >= 10.0 / (1.0 - p / 100.0)
                          for p in PERCENTILES}
    agg["_per_rep"] = per_rep
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
            r.pop("_per_rep", None)
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
            # A percentile the sample count cannot support is bracketed rather than
            # printed as though it were measured.
            v = f"{r[key]['median']:,}"
            if key.startswith("p") and not r["_resolvable"].get(key, True):
                v = f"({v})"
            row += f"{v:>12}"
        print(row)
        # Explicitly labelled: this is variation between repetitions inside one session.
        # It is not the uncertainty of a comparison against another configuration.
        spread = f"{'  within-session':<22}{'':>5}{'':>5}"
        for key in ["min", "p50", "p99", "p99.9", "p99.99", "max"]:
            spread += f"{r[key]['lo']:>5,}..{r[key]['hi']:<6,}"
        print(spread)
        if r["reps"] < 2:
            print(f"{'  ':<22}only {r['reps']} repetition: the spread above is not an "
                  f"uncertainty, it is one number repeated")
        if r["excluded_files"]:
            print(f"{'  ':<22}{r['excluded_files']} file(s) excluded as too short "
                  f"-- see warning above")
        unresolvable = [k for k, ok in r["_resolvable"].items() if not ok]
        if unresolvable:
            print(f"{'  ':<22}{r['samples_pooled']:,} samples pooled: "
                  f"{', '.join(unresolvable)} shown in parentheses, not supported")


if __name__ == "__main__":
    main()
