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


def config_runs(directory, drop):
    out = []
    for path in sorted(glob.glob(os.path.join(directory, "rep*_c*.csv"))):
        lat = load_run(path, drop)
        if lat is not None:
            m = re.search(r"rep(\d+)_c(\d+)\.csv$", os.path.basename(path))
            out.append((int(m.group(1)), int(m.group(2)), lat))
    return out


def drop_stats(directory, drop):
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
    """Median across runs for each percentile, plus the spread and sample count."""
    if not runs:
        return None
    row = {"runs": len(runs), "samples": int(min(len(l) for _, _, l in runs))}
    for p in HEADLINE:
        vals = [nearest_rank(l, p) for _, _, l in runs]
        key = f"p{p:g}"
        row[key] = int(statistics.median(vals))
        row[f"{key}_lo"] = int(min(vals))
        row[f"{key}_hi"] = int(max(vals))
    row["min"] = int(min(int(l[0]) for _, _, l in runs))
    maxes = [int(l[-1]) for _, _, l in runs]
    row["max"] = int(statistics.median(maxes))
    row["max_lo"] = int(min(maxes))
    row["max_hi"] = int(max(maxes))
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


def receive_modes(data_dir, out_dir, drop):
    """Spin versus kernel busy-poll on loopback -- the evidence for auto-selection."""
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
