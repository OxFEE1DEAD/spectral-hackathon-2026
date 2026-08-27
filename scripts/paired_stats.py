#!/usr/bin/env python3
"""Paired statistics over the blocks ab_bench.sh produced.

The point of a block design is that the comparison, not the absolute number, is what
repeats. So this reports differences *within* blocks and the spread of those differences
across blocks -- an uncertainty that contains clock drift and the environment's own mood,
which is the only kind worth quoting.

Three things it prints that the old per-rep spread could not:

  * `n` -- how many blocks actually contributed a valid pair. A comparison built on two
    blocks is labelled as such instead of reading like a measurement.
  * `null spread` -- how much the *reference arm alone* moves between blocks. If an effect
    is smaller than the reference's own between-block spread, it is not resolved, whatever
    the medians say. This is the column whose absence produced two withdrawn claims.
  * `sign p` -- an exact two-sided sign test on the per-block differences. It asks only
    whether the effect points the same way in most blocks, which needs no assumption about
    the shape of the noise and is honest about small n.

Metrics are also marked for whether they cross a clock boundary. A single-host leg
(source-ring wait, ring publish, receiving-kernel delivery) needs no offset correction and
is trustworthy in absolute terms; end-to-end and the wire leg are differences of two clocks
and are only trustworthy as within-block comparisons.

Usage: paired_stats.py BLOCKS_CSV [--reference ARM] [--json]
"""

import argparse
import csv
import json
import statistics
import sys
from math import comb

# name -> (label, crosses a clock boundary?)
METRICS = {
    # Loss and rescue come from counters on the receiving host alone, so they are exact and
    # entirely immune to the clock questions that limit every latency comparison here. On a
    # lossy path they are the headline, not the latency.
    "loss_pct":         ("first-copy loss %", False),
    "undelivered":      ("frames never delivered (post-gate)", False),
    "shm_p50":          ("source-ring wait p50", False),
    "rx_delivery_p50":  ("receiving-kernel delivery p50", False),
    "rx_delivery_p99":  ("receiving-kernel delivery p99", False),
    "wire_p50":         ("wire leg p50", True),
    "e2e_p50":          ("end-to-end p50", True),
    "e2e_p99":          ("end-to-end p99", True),
    "e2e_p999":         ("end-to-end p99.9", True),
    "e2e_p9999":        ("end-to-end p99.99", True),
}


def blocks_needed(alpha=0.05):
    """How many same-signed blocks a two-sided sign test needs to reach alpha.

    Worth stating because it is a hard floor on the design, not a detail. With every block
    pointing the same way the smallest attainable two-sided p is 2/2**n, so n=3 bottoms out
    at 0.250 and n=5 at 0.062: a three-block comparison cannot produce a significant result
    however large and however consistent the effect is. Six blocks is the first n that can.
    """
    n = 1
    while 2.0 / (2 ** n) > alpha:
        n += 1
    return n


MIN_BLOCKS = blocks_needed()


def sign_test(diffs):
    """Exact two-sided sign test. Ties are dropped, which is the conservative choice."""
    pos = sum(1 for d in diffs if d > 0)
    neg = sum(1 for d in diffs if d < 0)
    n = pos + neg
    if n == 0:
        return 1.0, 0
    k = min(pos, neg)
    tail = sum(comb(n, i) for i in range(0, k + 1)) / (2 ** n)
    return min(1.0, 2 * tail), n


def load(path):
    rows = []
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            rows.append(row)
    return rows


def num(row, key):
    v = row.get(key, "")
    if v in ("", None):
        return None
    try:
        return float(v)
    except ValueError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blocks_csv")
    ap.add_argument("--reference", default=None,
                    help="arm to compare the others against; default is the first arm seen")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    rows = load(args.blocks_csv)
    if not rows:
        print("no rows", file=sys.stderr)
        return 1

    invalid = [r for r in rows if r.get("valid") != "1"]
    valid = [r for r in rows if r.get("valid") == "1"]
    arms = list(dict.fromkeys(r["arm"] for r in rows))
    ref = args.reference or rows[0]["arm"]
    if ref not in arms:
        print(f"reference arm {ref!r} not present; arms are {arms}", file=sys.stderr)
        return 2

    n_blocks = len({r["block"] for r in rows})
    print(f"arms      : {', '.join(arms)}   (reference: {ref})")
    print(f"blocks    : {n_blocks} scheduled, "
          f"{len(valid)} of {len(rows)} arm-runs valid")
    if n_blocks < MIN_BLOCKS:
        print(f"WARNING   : {n_blocks} blocks cannot produce a significant sign test at any "
              f"effect size -- the floor is 2/2**{n_blocks} = {2/2**n_blocks:.3f}. "
              f"{MIN_BLOCKS} blocks is the minimum; everything below will read NOT resolved.")
    if invalid:
        # Named rather than counted: a dropped block is a fact about the measurement.
        for r in invalid:
            print(f"  dropped  : block {r['block']} arm {r['arm']}: {r.get('note','')}")

    # The entire argument for blocking is that arms within a block are close together in
    # time. That is an assumption about the control link, not a guarantee, so check it: a
    # flaky link can stretch a 90-second measurement into minutes of ssh retries, and a
    # block whose arms are twenty minutes apart is a sequential comparison wearing a block's
    # name.
    gaps = []
    for block in sorted({r["block"] for r in valid}):
        stamps = [num(r, "finished_at") for r in valid if r["block"] == block]
        stamps = [t for t in stamps if t]
        if len(stamps) >= 2:
            gaps.append((block, max(stamps) - min(stamps)))
    if gaps:
        worst = max(g for _, g in gaps)
        med_gap = statistics.median([g for _, g in gaps])
        print(f"arm spacing: median {med_gap/60:.1f} min within a block, worst "
              f"{worst/60:.1f} min"
              + ("" if worst <= 600 else "  <-- WARNING: at this spacing the clock has room "
                                         "to drift within a block, which is the effect "
                                         "blocking exists to remove"))

    by = {}
    for r in valid:
        by.setdefault(r["arm"], {})[r["block"]] = r

    report = {"reference": ref, "arms": {}}

    for metric, (label, cross) in METRICS.items():
        # The reference arm's own between-block spread: the resolution floor for this metric.
        ref_vals = [num(r, metric) for r in by.get(ref, {}).values()]
        ref_vals = [v for v in ref_vals if v is not None]
        if len(ref_vals) < 2:
            continue
        null_spread = max(ref_vals) - min(ref_vals)

        # Percentages live in the fractions of a percent, so a nanosecond-shaped format
        # rounds them to zero and the effect reads as "+0" while the sign test calls it
        # resolved -- which is exactly as confusing as it sounds.
        span = max(abs(v) for v in ref_vals) if ref_vals else 0
        prec = 4 if span < 100 else 0

        print()
        print(f"{label}   [{'cross-host, compare within block only' if cross else 'single clock, absolute'}]")
        print(f"  {ref+' (reference)':<26} median {statistics.median(ref_vals):>12,.{prec}f}"
              f"   between-block spread {null_spread:>11,.{prec}f}   n={len(ref_vals)}")

        for arm in arms:
            if arm == ref or arm not in by:
                continue
            diffs, pairs = [], 0
            for block, r in by[arm].items():
                a = num(r, metric)
                b = num(by[ref].get(block, {}), metric) if block in by[ref] else None
                if a is None or b is None:
                    continue
                diffs.append(a - b)
                pairs += 1
            if not diffs:
                continue
            med = statistics.median(diffs)
            p, n_used = sign_test(diffs)
            # An effect smaller than the reference's own wandering is not resolved, no
            # matter what the median difference says.
            resolved = abs(med) > null_spread and p <= 0.05
            if resolved:
                verdict = "resolved"
            elif abs(med) <= null_spread:
                verdict = "NOT resolved (smaller than the reference's own between-block spread)"
            elif n_used and (all(d > 0 for d in diffs) or all(d < 0 for d in diffs)):
                # Consistent direction but not enough blocks to prove it: say what is
                # missing, since "not resolved" otherwise reads as "no effect".
                verdict = (f"consistent in all {n_used} blocks but n too small "
                           f"(needs {MIN_BLOCKS})")
            else:
                verdict = f"NOT resolved (sign p={p:.3f}, direction inconsistent)"
            print(f"  {arm:<26} median diff {med:>+12,.{prec}f}   range "
                  f"{min(diffs):>+11,.{prec}f}..{max(diffs):>+11,.{prec}f}   n={pairs}  "
                  f"sign p={p:.3f}  {verdict}")
            report["arms"].setdefault(arm, {})[metric] = {
                "median_diff": med, "lo": min(diffs), "hi": max(diffs),
                "n": pairs, "sign_p": p, "null_spread": null_spread,
                "resolved": resolved, "cross_host": cross,
            }

    if args.json:
        print()
        print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
