#!/usr/bin/env python3
"""Turn one arm's raw summary into a single results row, with the validity checks applied.

Called by ab_bench.sh once per (block, arm). Kept separate from the runner because the
checks are the part that must not be skipped, and shell is the wrong place for "did this
measurement actually happen" logic.

The checks exist because every one of them has fired on a real run and would otherwise have
produced a plausible-looking number:

  * fewer samples than requested -- a truncated run reports percentiles from a fraction of
    the data, and a p99.99 from 400 events is not a p99.99.
  * nothing published -- the arm never ran at all (a stale process preflight refused it),
    which reads as a missing row rather than a zero.
  * suppression without redundancy -- something suppressed means a second stream was
    present, which halves the stream while nothing in the latency output looks wrong.
  * reorder with redundancy -- the redundant leg marks its copies, so reordering stays a
    live contamination detector even while redundancy is enabled.

A row that fails any of them is written with valid=0 and a note, never dropped: a silently
missing block is indistinguishable from a block that was never scheduled.
"""

import argparse
import json
import re
import sys

LEG_RE = re.compile(
    r"^(?P<name>\w+)\s+n=(?P<n>\d+)\s+p50=(?P<p50>\d+)\s+p99=(?P<p99>\d+)"
    r"\s+p99\.9=(?P<p999>\d+)\s+p99\.99=(?P<p9999>\d+)\s+max=(?P<max>\d+)")


def parse(raw_text):
    """Split the raw capture into its three sections and pull out what we need."""
    section = None
    gate, legs, e2e_json = {}, {}, []
    for line in raw_text.splitlines():
        if line.startswith("#gate"):
            section = "gate"; continue
        if line.startswith("#legs"):
            section = "legs"; continue
        if line.startswith("#e2e"):
            section = "e2e"; continue
        if section == "gate":
            m = re.match(r"\s*(published|suppressed|datagram reorder|malformed)\s*:\s*(\d+)", line)
            if m:
                gate[m.group(1)] = int(m.group(2))
            # "datagram gaps   : 107 (2204 datagrams)" -- the count that matters for a lossy
            # path is the second number, the datagrams actually missing, not the event count.
            m = re.match(r"\s*datagram gaps\s*:\s*(\d+)\s*\((\d+) datagram", line)
            if m:
                gate["gap_events"] = int(m.group(1))
                gate["lost_datagrams"] = int(m.group(2))
            m = re.match(r"\s*first-copy loss\s*:\s*([0-9.]+)%", line)
            if m:
                gate["loss_pct"] = float(m.group(1))
            # "  rescued       : N datagrams the original never delivered" -- the direct
            # measure of what redundancy buys, and it needs no clock at all.
            # Renamed from "rescued": a copy on its own path can simply win the race, so
            # this counts copies admitted before their original, not recoveries.
            m = re.match(r"\s*arrived first\s*:\s*(\d+)", line)
            if m:
                gate["dup_first"] = int(m.group(1))
            # "seq gaps : 7 (2124 frames never arrived)" -- the only honest measure of what
            # the consumer did not get. Unlike the loss counter it is measured after the
            # delivery gate, so it already accounts for whatever redundancy recovered.
            m = re.match(r"\s*seq gaps\s*:\s*(\d+)\s*\((\d+) frames never arrived", line)
            if m:
                gate["undelivered"] = int(m.group(2))
        elif section == "legs":
            m = LEG_RE.match(line.strip())
            if m:
                legs[m.group("name")] = {k: int(m.group(k)) for k in ("n", "p50", "p99", "p999", "p9999", "max")}
        elif section == "e2e":
            e2e_json.append(line)

    e2e = {}
    blob = "\n".join(e2e_json).strip()
    if blob:
        # summarize.py --json emits a list of aggregates; take the first, since ab_bench
        # measures one directory per arm.
        start = blob.find("[")
        if start >= 0:
            try:
                parsed = json.loads(blob[start:])
                if parsed:
                    e2e = parsed[0]
            except json.JSONDecodeError:
                e2e = {}
    return gate, legs, e2e


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True)
    ap.add_argument("--block", required=True)
    ap.add_argument("--arm", required=True)
    ap.add_argument("--rc", default="")
    ap.add_argument("--want-samples", type=int, default=0)
    # Tolerance on the sample count. A run is allowed to fall slightly short of the request
    # because the consumer stops on a count and the drop is applied afterwards.
    ap.add_argument("--sample-tolerance", type=float, default=0.90)
    ap.add_argument("--finished-at", default="", help="epoch second this arm-run completed")
    ap.add_argument("--elapsed-s", default="", help="wall-clock seconds this arm-run took")
    args = ap.parse_args()

    try:
        with open(args.raw) as handle:
            raw = handle.read()
    except OSError as exc:
        print(f"{args.block},{args.arm},0,,,,,,,,,,,,,,,,,"
              f"{args.finished_at},{args.elapsed_s},unreadable-raw:{exc.errno}")
        return

    gate, legs, e2e = parse(raw)

    published = gate.get("published", 0)
    suppressed = gate.get("suppressed", 0)
    reorder = gate.get("datagram reorder", 0)
    n = e2e.get("samples_per_run", 0)

    def leg(name, key):
        return legs.get(name, {}).get(key, "")

    def pctl(key):
        v = e2e.get(key)
        return v["median"] if isinstance(v, dict) else ""

    # Redundancy is inferred from the data rather than from the flags: if roughly one copy
    # was rejected per publication then a second stream was intended, and if none was then
    # any suppression at all is contamination.
    redundancy = published > 0 and suppressed > published // 4

    notes = []
    if args.rc not in ("0", ""):
        notes.append(f"rc={args.rc}")
    if published == 0:
        notes.append("no-data")
    if args.want_samples and n < args.want_samples * args.sample_tolerance:
        notes.append(f"short-run:{n}of{args.want_samples}")
    if not redundancy and suppressed > 0:
        notes.append(f"contaminated-suppressed={suppressed}")
    if redundancy and reorder > 0:
        notes.append(f"contaminated-reorder={reorder}")

    valid = 0 if notes else 1
    print(",".join(str(x) for x in [
        args.block, args.arm, valid, n, published, suppressed, reorder,
        gate.get("lost_datagrams", ""), gate.get("loss_pct", ""), gate.get("dup_first", ""),
        gate.get("undelivered", ""),
        leg("shm_ns", "p50"), leg("wire_ns", "p50"),
        leg("rx_delivery_ns", "p50"), leg("rx_delivery_ns", "p99"),
        pctl("p50"), pctl("p99"), pctl("p99.9"), pctl("p99.99"),
        args.finished_at, args.elapsed_s,
        ";".join(notes) if notes else "ok",
    ]))


if __name__ == "__main__":
    sys.exit(main())
