#!/usr/bin/env python3
"""Apply the measured clock offset to the cross-host columns, then re-run the sign test.

ab_bench.sh --offset-probe brackets each block with an idle clock_probe and writes the
two offsets to offsets.csv, but paired_stats.py deliberately leaves them unapplied: it
reports raw figures and says which ones are cross-host. This does the correction the
probe exists for.

The offset is not constant across a block -- these runs show it moving by up to 4.7 us
over 396 seconds -- so it is interpolated linearly between the bracketing probes and
evaluated at each arm's own midpoint. An end-to-end figure measured as
(receiver clock) - (sender clock) is transit plus that offset, so the offset is
subtracted. Single-clock columns are left alone; they never carried it.
"""
import csv
import sys
from collections import defaultdict

CROSS = ["wire_p50", "e2e_p50", "e2e_p99", "e2e_p999", "e2e_p9999"]


def sign_p(wins, n):
    """Two-sided sign test against a fair coin, exact."""
    from math import comb
    k = min(wins, n - wins)
    tail = sum(comb(n, i) for i in range(k + 1))
    return min(1.0, 2.0 * tail / 2 ** n)


def main(d):
    offs = {}
    with open(f"{d}/offsets.csv") as fh:
        for r in csv.reader(fh):
            if r and r[0].isdigit():
                offs[int(r[0])] = (float(r[1]), float(r[2]))

    rows = [r for r in csv.DictReader(open(f"{d}/blocks.csv")) if r["valid"] == "1"]
    by_block = defaultdict(list)
    for r in rows:
        by_block[int(r["block"])].append(r)

    corrected = defaultdict(dict)
    for b, arms in by_block.items():
        if b not in offs or len(arms) < 2:
            continue
        pre, post = offs[b]
        arms.sort(key=lambda r: int(r["finished_at"]))
        t0 = int(arms[0]["finished_at"]) - int(arms[0]["elapsed_s"])
        t1 = int(arms[-1]["finished_at"])
        span = max(1, t1 - t0)
        for r in arms:
            mid = int(r["finished_at"]) - int(r["elapsed_s"]) / 2.0
            off = pre + (post - pre) * (mid - t0) / span
            for c in CROSS:
                if r.get(c):
                    corrected[c].setdefault(b, {})[r["arm"]] = float(r[c]) - off

    names = sorted({a for c in corrected for b in corrected[c] for a in corrected[c][b]})
    ref, other = names[0], names[1]
    print(f"offset-corrected paired comparison   reference: {ref}   vs {other}\n")
    for c in CROSS:
        diffs = [corrected[c][b][other] - corrected[c][b][ref]
                 for b in sorted(corrected[c]) if len(corrected[c][b]) == 2]
        if not diffs:
            continue
        n = len(diffs)
        wins = sum(1 for x in diffs if x < 0)
        med = sorted(diffs)[n // 2] if n % 2 else 0.5 * (sorted(diffs)[n // 2 - 1] + sorted(diffs)[n // 2])
        p = sign_p(wins, n)
        verdict = "resolved" if p <= 0.05 else "NOT resolved"
        print(f"  {c:<12} median diff {med:+10,.0f} ns   range {min(diffs):+10,.0f}..{max(diffs):+10,.0f}"
              f"   {other} wins {wins}/{n}   sign p={p:.3f}  {verdict}")


if __name__ == "__main__":
    main(sys.argv[1])
