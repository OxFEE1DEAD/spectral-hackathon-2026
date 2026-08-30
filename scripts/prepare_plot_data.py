#!/usr/bin/env python3
"""Reduce raw latency samples to the small, plot-ready datasets the notebook reads.

The raw samples are large -- 30 million rows per configuration, hundreds of
megabytes each -- and there is no reason to commit them: every figure in the
notebook is built from aggregates, and the aggregates are a few kilobytes. What
lands in `data/plots/` is therefore a summary that is reviewable in a diff and
regenerable from these scripts.

Aggregates only. Nothing here emits per-message rows, which also means the
notebook's rendered output cannot smuggle the raw distribution back in through an
embedded plot data source.

Every figure is cross-host. Same-host runs are for checking that a code path works, never
for a number: measuring a network transport over loopback removes the driver, the NIC and
the wire, and an early revision of this work reported such figures as headline results,
understating latency about 15x. Directory names carry that guarantee -- a measurement
directory is prefixed with one of the cross-host run families below, and
assert_cross_host() refuses anything else rather than trusting the convention to hold.

Usage:
    prepare_plot_data.py [--data-dir data] [--out data/plots] [--drop 16384]
"""

import argparse
import csv
import glob
import os
import re
import statistics
import sys

import numpy as np

# Percentiles the task is judged on, plus the ones needed to draw a tail curve.
HEADLINE = [50.0, 90.0, 99.0, 99.9, 99.99, 99.999]
TAIL_GRID = [50, 75, 90, 95, 99, 99.5, 99.9, 99.95, 99.99, 99.995, 99.999,
             99.9995, 99.9999]

# Cross-host rate sweep: directory suffix -> offered rate.
RATE_TAGS = {"r100": 100, "r10k": 10_000, "r200k": 200_000,
             "r500k": 500_000, "r1m": 1_000_000, "r2m": 2_000_000}


# Run families, all cross-host. Anything in the data directory that does not start with
# one of these is not a measurement this script will read.
CROSS_HOST_PREFIXES = ("xh_", "xi_", "xa_", "xf_", "xb_", "xt_")


def assert_cross_host(directory):
    """Refuse to summarise a directory that is not from a cross-host run family.

    A guard rather than a comment, because the failure it prevents is silent: a
    same-host directory summarises perfectly happily and produces numbers roughly 15x
    too good, which look like a result rather than a mistake.
    """
    base = os.path.basename(os.path.normpath(directory))
    if not base.startswith(CROSS_HOST_PREFIXES):
        raise ValueError(
            f"refusing to read '{base}': not a cross-host run family "
            f"{CROSS_HOST_PREFIXES}. Every reported figure must be cross-host.")


def load_raw(path, drop):
    """Steady-state (seq, latency) from one run, in arrival order.

    The warm-up drop is capped at a tenth of the run: at 100 events/second a fixed
    16k-sample drop would discard the entire measurement.
    """
    import pandas as pd
    df = pd.read_csv(path, usecols=["seq", "latency_ns"], dtype=np.int64)
    seq = df["seq"].to_numpy()
    lat = df["latency_ns"].to_numpy()
    if seq.size == 0:
        return None, None
    d = min(drop, seq.size // 10)
    return np.array(seq[d:]), np.array(lat[d:])


def load_run(path, drop):
    """Steady-state latencies from one run, sorted."""
    _, lat = load_raw(path, drop)
    if lat is None or lat.size == 0:
        return None
    return np.sort(lat)


def nearest_rank(sorted_vals, p):
    """Match the harness's own percentile definition so numbers are comparable."""
    n = len(sorted_vals)
    rank = int(p / 100.0 * n)
    if rank < p / 100.0 * n:
        rank += 1
    return int(sorted_vals[max(1, min(rank, n)) - 1])


def pooled_rank(sorted_arrays, p):
    """Exact nearest-rank percentile of the concatenation of several sorted arrays,
    computed without concatenating them.

    Why this is not just np.concatenate: a ten-receiver, three-repetition run is ~40M
    int64 samples per array set, and pooling the biggest configurations in memory costs
    gigabytes. Each input is already sorted, so the rank of a candidate value across the
    whole pool is a sum of searchsorted() results, and a binary search over the value
    space finds the element at the target rank in ~40 iterations. The result is identical
    to sorting the pool, at no extra memory.

    Why pool at all: the alternative -- the median of the per-file percentiles -- discards
    exactly the files that contain the rare events, which is where a far-tail percentile
    lives. Measured on four real ten-consumer runs it understated p99.99 by 1.4x, 3.4x,
    8.2x and 1.7x.
    """
    arrays = [a for a in sorted_arrays if a is not None and len(a)]
    if not arrays:
        return None
    total = sum(len(a) for a in arrays)
    rank = int(p / 100.0 * total)
    if rank < p / 100.0 * total:
        rank += 1
    rank = max(1, min(rank, total))

    lo = min(int(a[0]) for a in arrays)
    hi = max(int(a[-1]) for a in arrays)
    # Invariant: the answer is in [lo, hi]. Both ends are attained values, so the search
    # terminates on an actual sample rather than an interpolated one.
    while lo < hi:
        mid = (lo + hi) // 2
        count = sum(int(np.searchsorted(a, mid, side="right")) for a in arrays)
        if count >= rank:
            hi = mid
        else:
            lo = mid + 1
    return int(lo)


def config_runs(directory, drop):
    assert_cross_host(directory)
    out = []
    for path in sorted(glob.glob(os.path.join(directory, "rep*_c*.csv"))):
        lat = load_run(path, drop)
        if lat is not None:
            m = re.search(r"rep(\d+)_c(\d+)\.csv$", os.path.basename(path))
            out.append((int(m.group(1)), int(m.group(2)), lat))
    return out


def drop_stats(directory, drop):
    assert_cross_host(directory)
    """Delivered-versus-expected, derived from gaps in the sequence column.

    Taken from the samples rather than parsed out of a log, so it cannot disagree
    with the latencies reported beside it.
    """
    recv = expected = 0
    for path in sorted(glob.glob(os.path.join(directory, "rep*_c*.csv"))):
        seq, _ = load_raw(path, drop)
        if seq is None or seq.size == 0:
            continue
        recv += int(seq.size)
        expected += int(seq[-1] - seq[0] + 1)
    if expected == 0:
        return None
    lost = max(0, expected - recv)
    return {"received": recv, "expected": expected, "dropped": lost,
            "drop_pct": round(100.0 * lost / expected, 6)}


def summarize(runs):
    """Pooled percentiles for each headline quantile, with a within-session spread.

    The headline value pools every sample in the configuration. The median of the per-file
    percentiles reads as a robust estimator and is the wrong one for a tail: the median
    across files throws away the files holding the rare events. On real runs the two
    disagree by up to 719% at p99.99.

    The *_lo/*_hi columns are the range of per-repetition pooled values. They describe
    variation between repetitions inside one session. They do not describe the drift of
    the two hosts' clocks between sessions, nor the environment's between-session mood --
    for a comparison between configurations use ab_bench.sh and paired_stats.py.
    """
    if not runs:
        return None
    row = {"runs": len(runs), "samples": int(min(len(l) for _, _, l in runs))}
    row["samples_pooled"] = int(sum(len(l) for _, _, l in runs))

    by_rep = {}
    for rep, _consumer, lat in runs:
        by_rep.setdefault(rep, []).append(lat)
    all_arrays = [lat for _, _, lat in runs]

    for p in HEADLINE:
        key = f"p{p:g}"
        row[key] = pooled_rank(all_arrays, p)
        per_rep = [pooled_rank(arrs, p) for arrs in by_rep.values()]
        per_rep = [v for v in per_rep if v is not None]
        row[f"{key}_lo"] = int(min(per_rep))
        row[f"{key}_hi"] = int(max(per_rep))

    row["min"] = int(min(int(l[0]) for _, _, l in runs))
    # The pooled maximum is the largest sample anywhere in the configuration, which is
    # what a maximum means. The median of the per-file maxima was a different quantity
    # wearing the same name.
    row["max"] = int(max(int(l[-1]) for _, _, l in runs))
    per_rep_max = [max(int(a[-1]) for a in arrs) for arrs in by_rep.values()]
    row["max_lo"] = int(min(per_rep_max))
    row["max_hi"] = int(max(per_rep_max))
    # A quantile needs roughly 10/(1-p) samples to mean anything; below that it is the
    # single worst observation wearing a percentile's name. Reported to the log rather than
    # added as a column, because twenty-five writers declare explicit field lists and a
    # silently widened row breaks all of them.
    unsupported = [f"p{p:g}" for p in HEADLINE
                   if row["samples_pooled"] < 10.0 / (1.0 - p / 100.0)]
    if unsupported:
        print(f"    note: {row['samples_pooled']:,} samples pooled -- "
              f"{', '.join(unsupported)} not supported by this sample count")
    row.pop("samples_pooled")
    return row


def write_csv(path, fieldnames, rows):
    with open(path, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=fieldnames)
        w.writeheader()
        for r in rows:
            w.writerow(r)
    print(f"  {path}  ({len(rows)} rows)")


def latency_vs_rate(data_dir, out_dir, drop):
    """Headline percentiles against offered rate, measured over the real network."""
    rows = []
    for suffix, rate in RATE_TAGS.items():
        d = os.path.join(data_dir, f"xh_{suffix}")
        runs = config_runs(d, drop)
        agg = summarize(runs)
        if not agg:
            continue
        row = {"rate": rate, **agg}
        ds = drop_stats(d, drop)
        if ds:
            row.update(ds)
        # A percentile needs roughly a hundred observations past it to mean
        # anything. Recorded per row so the notebook can grey out what is not
        # supported instead of drawing it as if it were.
        row["p99.999_supported"] = int(agg["samples"] >= 10_000_000)
        row["p99.99_supported"] = int(agg["samples"] >= 1_000_000)
        rows.append(row)
    rows.sort(key=lambda r: r["rate"])
    if not rows:
        return
    cols = (["rate", "runs", "samples", "min"] +
            [k for p in HEADLINE for k in (f"p{p:g}", f"p{p:g}_lo", f"p{p:g}_hi")] +
            ["max", "max_lo", "max_hi", "received", "expected", "dropped",
             "drop_pct", "p99.99_supported", "p99.999_supported"])
    for r in rows:
        for c in cols:
            r.setdefault(c, "")
    write_csv(os.path.join(out_dir, "latency_vs_rate.csv"), cols, rows)


def tail_curves(data_dir, out_dir, drop):
    """Latency at a grid of percentiles, one row per (rate, percentile).

    This is what a tail plot needs: enough of the curve to show its shape, at a
    fixed grid, without shipping the samples underneath it.
    """
    rows = []
    for suffix, rate in RATE_TAGS.items():
        d = os.path.join(data_dir, f"xh_{suffix}")
        runs = config_runs(d, drop)
        if not runs:
            continue
        for p in TAIL_GRID:
            vals = [nearest_rank(l, p) for _, _, l in runs]
            rows.append({"rate": rate, "percentile": p,
                         "latency_ns": int(statistics.median(vals))})
    rows.sort(key=lambda r: (r["rate"], r["percentile"]))
    if rows:
        write_csv(os.path.join(out_dir, "tail_curves.csv"),
                  ["rate", "percentile", "latency_ns"], rows)


def histogram(data_dir, out_dir, drop, rate=200000, bins=60):
    """Binned counts for one representative rate, clipped at p99.9 for display."""
    d = os.path.join(data_dir, "xh_r200k")
    runs = config_runs(d, drop)
    if not runs:
        return
    lat = np.concatenate([l for _, _, l in runs])
    clip = nearest_rank(np.sort(lat), 99.9)
    counts, edges = np.histogram(lat[lat <= clip], bins=bins)
    rows = [{"bin_low_ns": int(edges[i]), "bin_high_ns": int(edges[i + 1]),
             "count": int(counts[i])} for i in range(len(counts))]
    write_csv(os.path.join(out_dir, f"histogram_{rate}.csv"),
              ["bin_low_ns", "bin_high_ns", "count"], rows)


def _per_receiver(runs):
    """Group runs by receiver index and summarise each separately."""
    by_rx = {}
    for rep, rx, lat in runs:
        by_rx.setdefault(rx, []).append((rep, rx, lat))
    return {rx: summarize(v) for rx, v in sorted(by_rx.items())}


def _dirs_for(data_dir, prefix):
    return sorted(glob.glob(os.path.join(data_dir, prefix)))


def fanout(data_dir, out_dir, drop):
    """Latency against the number of *independent receivers*.

    Each receiver has its own socket, its own ring and its own consumer, and the
    sender transmits a separate datagram to every one of them. So this measures the
    cost of replicating to N destinations -- the thing that actually grows with
    receiver count -- rather than the cost of extra readers on one ring.

    Also emits the skew between receivers: because latency is measured at every
    receiver, the spread between the first-served and last-served destination is
    part of the result, not a detail.
    """
    rows, skew_rows = [], []
    for d in _dirs_for(data_dir, "xh_fan_n*"):
        n = int(re.search(r"n(\d+)$", os.path.basename(d)).group(1))
        runs = config_runs(d, drop)
        if not runs:
            continue
        agg = summarize(runs)          # pooled across receivers
        rows.append({"receivers": n, **agg})

        per_rx = _per_receiver(runs)
        for rx, a in per_rx.items():
            skew_rows.append({"receivers": n, "receiver": rx,
                              "p50": a["p50"], "p99": a["p99"],
                              "p99.9": a["p99.9"], "p99.99": a["p99.99"]})
        # Worst-to-best gap across receivers at each percentile: the fan-out skew.
        if len(per_rx) > 1:
            for key in ("p50", "p99"):
                vals = [a[key] for a in per_rx.values()]
                rows[-1][f"skew_{key}"] = max(vals) - min(vals)
        else:
            rows[-1]["skew_p50"] = 0
            rows[-1]["skew_p99"] = 0
    rows.sort(key=lambda r: r["receivers"])
    if rows:
        write_csv(os.path.join(out_dir, "fanout.csv"),
                  ["receivers", "runs", "samples", "min"] +
                  [k for p in HEADLINE for k in (f"p{p:g}", f"p{p:g}_lo", f"p{p:g}_hi")] +
                  ["max", "max_lo", "max_hi", "skew_p50", "skew_p99"], rows)
    if skew_rows:
        skew_rows.sort(key=lambda r: (r["receivers"], r["receiver"]))
        write_csv(os.path.join(out_dir, "fanout_per_receiver.csv"),
                  ["receivers", "receiver", "p50", "p99", "p99.9", "p99.99"],
                  skew_rows)


def send_methods(data_dir, out_dir, drop):
    """Compare the ways of replicating one datagram to N destinations.

    Reported per method: pooled percentiles across all receivers, and the skew
    between them. Fewer syscalls is not automatically better if it means the last
    destination is served materially later than the first.
    """
    rows = []
    for name in ("connected", "sendmmsg", "sendto"):
        d = os.path.join(data_dir, f"xh_m10_{name}")
        runs = config_runs(d, drop)
        if not runs:
            continue
        agg = summarize(runs)
        per_rx = _per_receiver(runs)
        row = {"method": name, **agg}
        for key in ("p50", "p99"):
            vals = [a[key] for a in per_rx.values()]
            row[f"skew_{key}"] = max(vals) - min(vals)
        rows.append(row)
    if rows:
        write_csv(os.path.join(out_dir, "send_methods.csv"),
                  ["method", "runs", "samples", "min"] +
                  [k for p in HEADLINE for k in (f"p{p:g}", f"p{p:g}_lo", f"p{p:g}_hi")] +
                  ["max", "max_lo", "max_hi", "skew_p50", "skew_p99"], rows)


def backends(data_dir, out_dir, drop):
    """io_uring against kernel UDP, with each end varied independently.

    The backend can be selected separately at the sender and the receiver, so this is a
    factorial rather than a single on/off comparison. That matters because the two
    directions have different amounts to gain: replicating a datagram to N destinations
    costs N system calls on the kernel-UDP path and one through a submission ring, while
    the receive path already costs one call per datagram either way and so has nothing
    to save.

    Loss is carried alongside latency because on this comparison it is not incidental:
    the slower receive path also drops, and reporting the latency without that would
    flatter it.
    """
    cfgs = [("udp+udp", "xi_A_udp_udp", "udp", "udp"),
            ("iouring+udp", "xi_B_iou_udp", "iouring", "udp"),
            ("sqpoll+udp", "xi_C_sqpoll_udp", "iouring-sqpoll", "udp"),
            ("udp+iouring", "xi_D2_udp_iou", "udp", "iouring"),
            ("iouring+iouring", "xi_E2_iou_iou", "iouring", "iouring")]
    rows = []
    for label, tag, send, recv in cfgs:
        d = os.path.join(data_dir, tag)
        runs = config_runs(d, drop)
        if not runs:
            continue
        agg = summarize(runs)
        row = {"config": label, "send": send, "recv": recv, **agg}
        stats = drop_stats(d, drop)
        row["drop_pct"] = stats["drop_pct"] if stats else ""
        per_rx = _per_receiver(runs)
        for key in ("p50", "p99"):
            vals = [a[key] for a in per_rx.values()]
            row[f"skew_{key}"] = max(vals) - min(vals) if len(vals) > 1 else 0
        rows.append(row)
    if rows:
        write_csv(os.path.join(out_dir, "backends.csv"),
                  ["config", "send", "recv", "runs", "samples", "min"] +
                  [k for p in HEADLINE for k in (f"p{p:g}", f"p{p:g}_lo", f"p{p:g}_hi")] +
                  ["max", "max_lo", "max_hi", "skew_p50", "skew_p99", "drop_pct"], rows)


def wire_leg_causation(data_dir, out_dir, drop):
    """Is the wire-leg tail caused upstream of the sender's socket, or downstream of it?

    Two questions, both answerable from the staged per-message legs without any new run.

    Correlation between the source-ring wait and the wire leg asks whether a message that
    waited a long time for the sender also takes a long time to arrive. If the sender were
    the cause -- its own congestion spilling into transmit -- the two would move together.

    Clustering asks whether the worst wire legs are isolated events or runs of consecutive
    messages. A run means one stall caught everything in flight behind it, which is the
    signature of a queue draining rather than of independent bad luck per packet.

    Reported per send mechanism, so a mechanism-dependent answer would be visible. The
    per-mechanism *thresholds* are deliberately not compared: stage capture runs on one
    receiver out of ten, so which absolute tail it sees depends on which receiver caught a
    burst, and that is luck rather than a property of the mechanism.
    """
    import numpy as np
    import pandas as pd
    rows = []
    for label, tag in (("kernel-udp", "xa_udp_n10"), ("io_uring", "xa_iou_n10"),
                       ("afxdp-copy", "xa_xdp_n10")):
        path = os.path.join(data_dir, tag, "stages.csv")
        if not os.path.exists(path):
            continue
        df = pd.read_csv(path, usecols=["shm_ns", "wire_ns"], dtype=np.int64,
                         skiprows=range(1, drop + 1))
        shm = df["shm_ns"].to_numpy()
        wire = df["wire_ns"].to_numpy()
        n = wire.size
        if n < 1000:
            continue
        thr = float(np.quantile(wire, 0.9999))
        idx = np.flatnonzero(wire > thr)
        gaps = np.diff(idx) if idx.size > 1 else np.array([], dtype=np.int64)
        bursts = 1 + int((gaps != 1).sum()) if idx.size else 0
        rows.append({
            "backend": label,
            "samples": int(n),
            "corr_shm_wire": round(float(np.corrcoef(shm, wire)[0, 1]), 4),
            "shm_p50_all": int(np.median(shm)),
            # If the sender were the cause, this would be well above shm_p50_all.
            "shm_p50_worst_wire": int(np.median(shm[wire > np.quantile(wire, 0.999)])),
            "spike_threshold_ns": int(thr),
            "spikes": int(idx.size),
            "bursts": bursts,
            "msgs_per_burst": round(idx.size / bursts, 1) if bursts else 0,
            "adjacent_pct": round(100.0 * int((gaps == 1).sum()) / idx.size, 1)
                            if idx.size else 0,
        })
        del df
    if rows:
        write_csv(os.path.join(out_dir, "wire_leg_causation.csv"),
                  ["backend", "samples", "corr_shm_wire", "shm_p50_all",
                   "shm_p50_worst_wire", "spike_threshold_ns", "spikes", "bursts",
                   "msgs_per_burst", "adjacent_pct"], rows)


def wire_leg_split(data_dir, out_dir, drop):
    """Split the wire leg at the receiver's kernel boundary, across message rates.

    The wire leg dominates the total and absorbed every send-side improvement, but it was
    one opaque number covering sender transmit, both NICs, the fabric and receiver receive.
    Kernel receive timestamps (SO_TIMESTAMPING, RX_SOFTWARE) cut it in two:

      to_rx_stamp   sender's pre-send stamp -> the kernel stamping the packet on entry to
                    its receive path. Sender transmit, both NICs, the fabric and the
                    receiving driver, still together. Spans two clocks.
      rx_delivery   that stamp -> our recv returning. The receiving kernel's delivery path:
                    protocol demux, socket queue, busy-poll pickup. Two readings of one
                    clock on one host, so this one is exact.

    The second is what decides whether a kernel-bypass receive path has anything to win.

    Measured across rates because the drain events that dominate the far tail only appear
    at high rates, so a decomposition at one rate would miss the regime the tail lives in.
    Each rate is measured twice, with timestamping on and off, because asking for
    timestamps turns recv() into recvmsg() with control data and perturbs the path being
    measured; the pair prices that perturbation instead of assuming it away.
    """
    import numpy as np
    import pandas as pd

    def legs(tag):
        path = os.path.join(data_dir, tag, "stages.csv")
        if not os.path.exists(path):
            return None
        head = pd.read_csv(path, nrows=1)
        cols = [c for c in ("shm_ns", "wire_ns", "publish_ns", "rx_delivery_ns")
                if c in head.columns]
        # A larger warm-up drop than the latency samples get, and for a reason specific to
        # this instrumentation: the receiver preallocates and page-touches its stage buffer
        # at startup -- hundreds of megabytes when the capacity is set for a high-rate run
        # -- and datagrams queue behind that. Measured on the ten-destination run, every
        # value above a millisecond sat in the first 39,284 rows and nowhere else, so the
        # fixed 16k drop left a pure startup artefact in the tail. One percent of the file
        # removes it and costs nothing at these sample counts.
        total = sum(1 for _ in open(path)) - 1
        skip = max(drop, total // 100)
        df = pd.read_csv(path, usecols=cols, dtype=np.int64,
                         skiprows=range(1, skip + 1))
        n = len(df)
        if n == 0:
            return None
        out = {"samples": n}
        for col in ("shm_ns", "wire_ns", "publish_ns"):
            a = np.sort(df[col].to_numpy())
            k = col[:-3]
            out[f"{k}_p50"] = int(a[int(0.50 * n)])
            out[f"{k}_p99"] = int(a[int(0.99 * n)])
            out[f"{k}_p999"] = int(a[int(0.999 * n)])
        if "rx_delivery_ns" in cols:
            rx = df["rx_delivery_ns"].to_numpy()
            # A datagram the kernel did not stamp contributes zero; excluding those keeps
            # the distribution from being dragged down by a reporting gap rather than by
            # a real measurement.
            rx = np.sort(rx[rx > 0])
            out["rx_samples"] = int(rx.size)
            if rx.size:
                out["rx_delivery_p50"] = int(rx[int(0.50 * rx.size)])
                out["rx_delivery_p99"] = int(rx[int(0.99 * rx.size)])
                out["rx_delivery_p999"] = int(rx[int(0.999 * rx.size)])
        del df
        return out

    RATES = [("200k", 200_000), ("500k", 500_000), ("1m", 1_000_000), ("2m", 2_000_000)]
    rows = []
    for label, rate in RATES:
        ts = legs(f"xt_ts_r{label}")
        pl = legs(f"xt_pl_r{label}")
        if ts is None:
            continue
        row = {"rate": rate, "receivers": 1, "samples": ts["samples"]}
        for k in ("shm_p50", "wire_p50", "publish_p50", "wire_p99", "wire_p999"):
            row[k] = ts.get(k, "")
        for k in ("rx_samples", "rx_delivery_p50", "rx_delivery_p99", "rx_delivery_p999"):
            row[k] = ts.get(k, "")
        if row.get("rx_delivery_p50") not in ("", None):
            # What is left of the wire leg once the receiving kernel is taken out.
            row["to_rx_stamp_p50"] = row["wire_p50"] - row["rx_delivery_p50"]
            row["rx_share_of_wire_pct"] = round(
                100.0 * row["rx_delivery_p50"] / row["wire_p50"], 2)
        # The cost of measuring: same configuration, timestamping off.
        if pl is not None:
            row["plain_wire_p50"] = pl["wire_p50"]
            row["tstamp_overhead_p50"] = ts["wire_p50"] - pl["wire_p50"]
        rows.append(row)

    # The fan-out point, for continuity with the backend comparison.
    n10 = legs("xt_ts_n10")
    if n10 is not None:
        row = {"rate": 200_000, "receivers": 10, "samples": n10["samples"]}
        for k in ("shm_p50", "wire_p50", "publish_p50", "wire_p99", "wire_p999",
                  "rx_samples", "rx_delivery_p50", "rx_delivery_p99", "rx_delivery_p999"):
            row[k] = n10.get(k, "")
        if row.get("rx_delivery_p50") not in ("", None):
            row["to_rx_stamp_p50"] = row["wire_p50"] - row["rx_delivery_p50"]
            row["rx_share_of_wire_pct"] = round(
                100.0 * row["rx_delivery_p50"] / row["wire_p50"], 2)
        rows.append(row)

    if rows:
        cols = ["rate", "receivers", "samples", "shm_p50", "wire_p50", "wire_p99",
                "wire_p999", "publish_p50", "rx_samples", "rx_delivery_p50",
                "rx_delivery_p99", "rx_delivery_p999", "to_rx_stamp_p50",
                "rx_share_of_wire_pct", "plain_wire_p50", "tstamp_overhead_p50"]
        for r in rows:
            for c in cols:
                r.setdefault(c, "")
        write_csv(os.path.join(out_dir, "wire_leg_split.csv"), cols, rows)


def send_backend_stages(data_dir, out_dir, drop):
    """Where each send mechanism spends its time, and where the drift lives.

    This is the measurement that made sense of the backend comparison. Latency is split
    at the two points the relay controls:

      shm     producer stamp -> the sender's pre-send stamp. One host, one clock. This is
              how long a message waited in the source ring, so it is the leg a cheaper
              send is supposed to shorten.
      wire    pre-send -> arrival at the receiver. Spans both hosts and the whole
              transmit and receive path.
      publish arrival -> published into the receiver's ring.

    Two things fall out. The shm leg is cleanly ordered by send mechanism and is stable
    across sessions. The wire leg is far larger, and it is where the between-session
    drift lives -- which is why end-to-end medians could not separate two backends
    differing by a few microseconds, and why the shm leg is the honest place to look.
    """
    import numpy as np
    import pandas as pd
    groups = [("kernel-udp", "xa_udp_n10"), ("io_uring", "xa_iou_n10"),
              ("afxdp-copy", "xa_xdp_n10")]
    rows = []
    for label, tag in groups:
        path = os.path.join(data_dir, tag, "stages.csv")
        if not os.path.exists(path):
            continue
        df = pd.read_csv(path, usecols=["shm_ns", "wire_ns", "publish_ns"],
                         dtype=np.int64, skiprows=range(1, drop + 1))
        n = len(df)
        if n == 0:
            continue
        row = {"backend": label, "samples": n}
        for col, name in (("shm_ns", "shm"), ("wire_ns", "wire"),
                          ("publish_ns", "publish")):
            a = np.sort(df[col].to_numpy())
            row[f"{name}_p50"] = int(a[int(0.50 * n)])
            row[f"{name}_p99"] = int(a[int(0.99 * n)])
        row["total_p50"] = row["shm_p50"] + row["wire_p50"] + row["publish_p50"]
        rows.append(row)
        del df
    if rows:
        write_csv(os.path.join(out_dir, "send_backend_stages.csv"),
                  ["backend", "samples", "shm_p50", "shm_p99", "wire_p50", "wire_p99",
                   "publish_p50", "publish_p99", "total_p50"], rows)


def send_backend_drain(out_dir):
    """Frames per datagram, every run measured, as a send-cost proxy that actually repeats.

    At a fixed offered rate this is a direct measure of how fast the sender empties its
    source ring: the interval between two datagram sends is frames/datagram divided by
    the rate. It needs no cross-host clock and no session-to-session comparison, which is
    why it reproduced to within a few percent where end-to-end medians did not.

    The figures are transcribed from the sender's own report over twelve runs across three
    sessions rather than recomputed, because the sender counts them exactly.
    """
    runs = {"kernel-udp": [2.24, 2.28, 2.31, 2.32, 2.35],
            "io_uring": [2.04, 2.05, 2.06, 2.10],
            "afxdp-copy": [1.44, 1.45, 1.45]}
    rate = 200_000
    rows = []
    for backend, vals in runs.items():
        mean = sum(vals) / len(vals)
        rows.append({"backend": backend, "runs": len(vals),
                     "frames_per_datagram_mean": round(mean, 3),
                     "frames_per_datagram_min": min(vals),
                     "frames_per_datagram_max": max(vals),
                     "send_cycle_ns": int(round(mean / rate * 1e9))})
    write_csv(os.path.join(out_dir, "send_backend_drain.csv"),
              ["backend", "runs", "frames_per_datagram_mean", "frames_per_datagram_min",
               "frames_per_datagram_max", "send_cycle_ns"], rows)


def backend_reps(data_dir, out_dir, drop):
    """Per-repetition pooled percentiles, which is the statistic this comparison needs.

    summarize() reports the median of the *per-file* percentiles. That is a robust
    estimator of a typical receiver, but it is the wrong tool for asking whether two
    backends differ in the tail: with ten receivers and three repetitions it produces a
    spread that reflects which individual file happened to catch a drain event, and at
    p99.99 that spread ran from 70 us to 15 ms for every configuration -- swamping any
    difference between them.

    Pooling the ten receivers *within* a repetition instead gives one value per
    repetition backed by the full ~40M samples, which is both the physically meaningful
    quantity (the percentile of the delivered stream) and well enough resolved to say
    whether three repetitions of one backend separate from three of another. Emitting
    every repetition rather than a summary is deliberate: overlap is the finding
    wherever it occurs, and a mean would hide it.
    """
    import numpy as np
    groups = [("xi_A_udp_udp", "udp+udp"), ("xi_B_iou_udp", "iouring+udp"),
              ("xi_C_sqpoll_udp", "sqpoll+udp"), ("xi_D2_udp_iou", "udp+iouring"),
              ("xi_E2_iou_iou", "iouring+iouring")]
    rows = []
    for tag, label in groups:
        by_rep = {}
        for path in sorted(glob.glob(os.path.join(data_dir, tag, "rep*_c*.csv"))):
            m = re.search(r"rep(\d+)_c(\d+)\.csv$", os.path.basename(path))
            by_rep.setdefault(int(m.group(1)), []).append(path)
        for rep, paths in sorted(by_rep.items()):
            arrs = [a for a in (load_run(p, drop) for p in paths) if a is not None]
            if not arrs:
                continue
            pooled = np.sort(np.concatenate(arrs))
            row = {"config": label, "rep": rep, "samples": int(pooled.size)}
            for p in HEADLINE:
                row[f"p{p:g}"] = nearest_rank(pooled, p)
            rows.append(row)
            del arrs, pooled
    if rows:
        write_csv(os.path.join(out_dir, "backend_reps.csv"),
                  ["config", "rep", "samples"] + [f"p{p:g}" for p in HEADLINE], rows)


def backend_fanout(data_dir, out_dir, drop):
    """The send-path advantage against destination count.

    This is the arithmetic being tested: N system calls versus one. The gap should widen
    with N and vanish at N=1, where a single connected send() and a single submission
    are the same one transition into the kernel.
    """
    rows = []
    for backend, prefix in (("udp", "xf_udp_n*"), ("iouring", "xf_iou_n*")):
        for d in _dirs_for(data_dir, prefix):
            n = int(re.search(r"n(\d+)$", os.path.basename(d)).group(1))
            runs = config_runs(d, drop)
            if not runs:
                continue
            agg = summarize(runs)
            per_rx = _per_receiver(runs)
            vals = [a["p50"] for a in per_rx.values()]
            rows.append({"backend": backend, "receivers": n, **agg,
                         "skew_p50": max(vals) - min(vals) if len(vals) > 1 else 0})
    rows.sort(key=lambda r: (r["backend"], r["receivers"]))
    if rows:
        write_csv(os.path.join(out_dir, "backend_fanout.csv"),
                  ["backend", "receivers", "runs", "samples", "min"] +
                  [k for p in HEADLINE for k in (f"p{p:g}", f"p{p:g}_lo", f"p{p:g}_hi")] +
                  ["max", "max_lo", "max_hi", "skew_p50"], rows)


def receive_modes(data_dir, out_dir, drop):
    """Spin versus kernel busy-poll, cross-host over the NIC.

    The evidence for deriving the receive mode from the path rather than exposing it. The
    docstring here previously said "on loopback", which was simply wrong about its own
    inputs: these tags are cross-host runs, as every reported figure is.
    """
    rows = []
    for name, label in (("xh_mode_spin", "spin"), ("xh_mode_busypoll", "busy-poll")):
        d = os.path.join(data_dir, name)
        s = summarize(config_runs(d, drop))
        if s:
            rows.append({"mode": label, **s})
    if rows:
        write_csv(os.path.join(out_dir, "receive_modes.csv"),
                  ["mode", "runs", "samples", "min"] +
                  [k for p in HEADLINE for k in (f"p{p:g}", f"p{p:g}_lo", f"p{p:g}_hi")] +
                  ["max", "max_lo", "max_hi"], rows)


def scatter(data_dir, out_dir, drop, tag="xh_r200k", keep=2500,
            outlier_cap=3000):
    """Decimated latency-versus-position samples, per repetition, plus every outlier.

    A scatter is the only view that shows *when* outliers happen and whether they
    cluster, which percentiles cannot. Committing 60M raw points is not an option,
    so each repetition contributes an evenly spaced sample of the baseline plus
    every point above that repetition's p99.9 -- so the outlier structure survives
    decimation intact while the file stays small.
    """
    rows = []
    for path in sorted(glob.glob(os.path.join(data_dir, tag, "rep*_c0.csv"))):
        rep = int(re.search(r"rep(\d+)_", os.path.basename(path)).group(1))
        seq, lat = load_raw(path, drop)
        if seq is None or lat.size == 0:
            continue
        n = lat.size
        thresh = int(np.quantile(lat, 0.999))
        base_idx = np.linspace(0, n - 1, min(keep, n)).astype(np.int64)
        out_idx = np.flatnonzero(lat > thresh)
        if out_idx.size > outlier_cap:  # keep the largest, note the truncation
            out_idx = out_idx[np.argsort(lat[out_idx])[-outlier_cap:]]
        for i in np.union1d(base_idx, out_idx):
            rows.append({"rep": rep,
                         "position": int(i),
                         "fraction": round(float(i) / max(1, n - 1), 6),
                         "latency_ns": int(lat[i]),
                         "outlier": int(lat[i] > thresh)})
    if rows:
        rows.sort(key=lambda r: (r["rep"], r["position"]))
        write_csv(os.path.join(out_dir, "scatter_200k.csv"),
                  ["rep", "position", "fraction", "latency_ns", "outlier"], rows)


def burst_stats(data_dir, out_dir, drop, tag="xh_r200k",
                thresholds=(50_000, 100_000, 200_000, 500_000)):
    """Are latency spikes isolated events, or do they arrive in runs?

    For each magnitude threshold, group the messages above it into runs of
    consecutive arrivals. If spikes were independent, almost every run would have
    length one. The share that are singletons versus the share sitting in runs of ten
    or more is therefore a direct measure of burstiness -- and it turns out to depend
    strongly on how large the spike is.
    """
    rows = []
    for path in sorted(glob.glob(os.path.join(data_dir, tag, "rep*_c0.csv"))):
        rep = int(re.search(r"rep(\d+)_", os.path.basename(path)).group(1))
        _, lat = load_raw(path, drop)
        if lat is None or lat.size == 0:
            continue
        for thr in thresholds:
            idx = np.flatnonzero(lat > thr)
            if idx.size == 0:
                rows.append({"rep": rep, "threshold_ns": thr, "messages": 0,
                             "bursts": 0, "largest_burst": 0,
                             "singleton_pct": "", "in_bursts_ge10_pct": ""})
                continue
            runs = np.split(idx, np.flatnonzero(np.diff(idx) > 1) + 1)
            lens = np.array([len(r) for r in runs])
            rows.append({
                "rep": rep, "threshold_ns": int(thr), "messages": int(idx.size),
                "bursts": int(len(runs)), "largest_burst": int(lens.max()),
                "singleton_pct": round(100.0 * lens[lens == 1].sum() / lens.sum(), 2),
                "in_bursts_ge10_pct": round(100.0 * lens[lens >= 10].sum() / lens.sum(), 2),
            })
    if rows:
        rows.sort(key=lambda r: (r["threshold_ns"], r["rep"]))
        write_csv(os.path.join(out_dir, "burst_stats.csv"),
                  ["rep", "threshold_ns", "messages", "bursts", "largest_burst",
                   "singleton_pct", "in_bursts_ge10_pct"], rows)


def stages(data_dir, out_dir, tag="xh_stage_full"):
    """Per-leg timings, joined to end-to-end on seq so every leg is comparable.

    Four legs, and the join is what makes the last one available: the receiver knows
    when it published, the consumer knows when it read, and matching on seq_id gives
    the gap between them without either side needing the other's clock.
    """
    import pandas as pd
    spath = os.path.join(data_dir, tag, "stages.csv")
    epath = os.path.join(data_dir, tag, "rep1_c0.csv")
    if not (os.path.exists(spath) and os.path.exists(epath)):
        return
    st = pd.read_csv(spath)
    e2 = pd.read_csv(epath, usecols=["seq", "latency_ns"], dtype=np.int64)
    m = st.merge(e2, on="seq", how="inner")
    if m.empty:
        return
    m["post_ns"] = m["latency_ns"] - (m["shm_ns"] + m["wire_ns"] + m["publish_ns"])

    legs = [("shm_ns", "source ring + sender"),
            ("wire_ns", "wire (kernel TX to arrival)"),
            ("publish_ns", "arrival to published"),
            ("post_ns", "published to consumer read"),
            ("latency_ns", "end to end")]
    rows = []
    for col, label in legs:
        v = np.sort(np.array(m[col].to_numpy()))
        row = {"stage": label, "samples": int(v.size), "min": int(v[0])}
        for p in HEADLINE:
            row[f"p{p:g}"] = int(np.quantile(v, p / 100.0))
        row["p99.9999"] = int(np.quantile(v, 0.999999))
        row["max"] = int(v[-1])
        rows.append(row)
    write_csv(os.path.join(out_dir, "stage_breakdown.csv"),
              ["stage", "samples", "min"] + [f"p{p:g}" for p in HEADLINE] +
              ["p99.9999", "max"], rows)

    # Which leg owns the outliers. This is the whole point of the decomposition, so
    # it is emitted as data rather than left as a sentence in the write-up.
    over = m[m["latency_ns"] > 1_000_000]
    if len(over):
        dom = over[["shm_ns", "wire_ns", "publish_ns", "post_ns"]].idxmax(axis=1)
        rows = [{"leg": leg, "count": int(n),
                 "share_pct": round(100.0 * n / len(over), 2)}
                for leg, n in dom.value_counts().items()]
        write_csv(os.path.join(out_dir, "outlier_attribution.csv"),
                  ["leg", "count", "share_pct"], rows)

    # One drain event, message by message: consecutive sequence ids whose latency
    # falls by roughly the inter-message interval. This is the shape that identifies
    # the outliers as a queue draining rather than independent random stalls.
    if len(over):
        worst = int(over.nlargest(1, "latency_ns")["seq"].iloc[0])
        win = m[(m["seq"] >= worst - 20) & (m["seq"] <= worst + 420)]
        rows = [{"offset": int(r.seq) - worst, "latency_ns": int(r.latency_ns),
                 "wire_ns": int(r.wire_ns)} for r in win.itertuples()]
        if rows:
            write_csv(os.path.join(out_dir, "drain_event.csv"),
                      ["offset", "latency_ns", "wire_ns"], rows)


def loss_summary(data_dir, out_dir, drop):
    """Loss actually observed on the L2 path, per rate, from sequence gaps."""
    rows = []
    for suffix, rate in RATE_TAGS.items():
        ds = drop_stats(os.path.join(data_dir, f"xh_{suffix}"), drop)
        if ds:
            rows.append({"rate": rate, **ds})
    rows.sort(key=lambda r: r["rate"])
    if rows:
        write_csv(os.path.join(out_dir, "loss_summary.csv"),
                  ["rate", "received", "expected", "dropped", "drop_pct"], rows)


def burst_loss(out_dir):
    """How the loss that does occur is distributed, and what redundancy recovered.

    Taken from the receiver's own counters during the duplication sweep (gap events
    versus datagrams lost, and copies that carried data their original never
    delivered). Recorded here so the numbers in the write-up have a checkable source.
    """
    rows = [
        {"rate": 500_000, "duplication_pct": 77.7, "gap_events": 7,
         "datagrams_lost": 485, "rescued_by_copy": 2},
        {"rate": 1_000_000, "duplication_pct": 0.4, "gap_events": 50,
         "datagrams_lost": 4007, "rescued_by_copy": 14},
    ]
    for r in rows:
        r["datagrams_per_event"] = round(r["datagrams_lost"] / r["gap_events"], 1)
        r["rescue_pct"] = round(100.0 * r["rescued_by_copy"] / r["datagrams_lost"], 2)
    write_csv(os.path.join(out_dir, "burst_loss.csv"),
              ["rate", "duplication_pct", "gap_events", "datagrams_lost",
               "datagrams_per_event", "rescued_by_copy", "rescue_pct"], rows)


def pps_budget(out_dir, ceiling=840_000, msgs_per_datagram=6.0):
    """The packet-rate budget shared across receivers.

    Each receiver costs one more packet per datagram, so the measured pps ceiling
    divides the achievable message rate by the receiver count. This is the binding
    fan-out limit, and it does not depend on how fast the sender's code is.
    """
    rows = [{"receivers": n,
             "datagram_budget": int(ceiling / n),
             "message_rate": int(ceiling / n * msgs_per_datagram)}
            for n in (1, 2, 3, 10, 20, 50)]
    write_csv(os.path.join(out_dir, "pps_budget.csv"),
              ["receivers", "datagram_budget", "message_rate"], rows)


def message_sizes(out_dir):
    """Message footprint before and after the format rework.

    Static figures, kept here so the notebook has a single source for them and the
    numbers in the write-up cannot drift away from the ones plotted.
    """
    rows = [
        {"message": "Trade", "before_bytes": 192, "after_bytes": 80},
        {"message": "Bbo", "before_bytes": 192, "after_bytes": 64},
        {"message": "OrderBook", "before_bytes": 576, "after_bytes": 160},
        {"message": "mixed average", "before_bytes": 320, "after_bytes": 101},
        {"message": "shm ring slot", "before_bytes": 640, "after_bytes": 192},
    ]
    write_csv(os.path.join(out_dir, "message_sizes.csv"),
              ["message", "before_bytes", "after_bytes"], rows)


def copy_adaptive(data_dir, out_dir):
    """Sender-side duplication and batching counters, already small."""
    src = os.path.join(data_dir, "dup_sweep.csv")
    if not os.path.exists(src):
        return
    with open(src) as fh:
        rows = [r for r in csv.DictReader(fh)
                if r.get("rate") and r.get("frames_per_datagram")]
    rows.sort(key=lambda r: int(r["rate"]))
    if rows:
        write_csv(os.path.join(out_dir, "adaptive_redundancy.csv"),
                  list(rows[0].keys()), rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data-dir", default="data")
    ap.add_argument("--out", default="data/plots")
    ap.add_argument("--drop", type=int, default=16384,
                    help="samples discarded at the start of each run")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    print("writing plot-ready aggregates:")
    latency_vs_rate(args.data_dir, args.out, args.drop)
    tail_curves(args.data_dir, args.out, args.drop)
    histogram(args.data_dir, args.out, args.drop)
    scatter(args.data_dir, args.out, args.drop)
    burst_stats(args.data_dir, args.out, args.drop)
    stages(args.data_dir, args.out)
    fanout(args.data_dir, args.out, args.drop)
    send_methods(args.data_dir, args.out, args.drop)
    backends(args.data_dir, args.out, args.drop)
    backend_reps(args.data_dir, args.out, args.drop)
    send_backend_stages(args.data_dir, args.out, args.drop)
    send_backend_drain(args.out)
    wire_leg_causation(args.data_dir, args.out, args.drop)
    wire_leg_split(args.data_dir, args.out, args.drop)
    backend_fanout(args.data_dir, args.out, args.drop)
    receive_modes(args.data_dir, args.out, args.drop)
    loss_summary(args.data_dir, args.out, args.drop)
    burst_loss(args.out)
    pps_budget(args.out)
    copy_adaptive(args.data_dir, args.out)
    message_sizes(args.out)

    total = sum(os.path.getsize(os.path.join(args.out, f))
                for f in os.listdir(args.out))
    print(f"total {total / 1024:.1f} KiB in {args.out}")


if __name__ == "__main__":
    main()
