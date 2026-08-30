#!/usr/bin/env python3
"""Per-receiver skew from a fan-out A/B, which blocks.csv cannot show.

ab_row.py pools every consumer in an arm-run into one end-to-end figure, and the
quantity fan-out is judged on is the difference *between* receivers -- pooling
averages exactly the thing being measured away. bench.sh does keep the per-consumer
files, so the skew is recoverable from those.

Usage:  fanout_skew.py HOST REMOTE_DATA_DIR [TAG_GLOB]   (one row per arm-run)
"""
import re
import subprocess
import sys

REMOTE = r'''
import csv, glob, os, re, statistics
base, pat = "%s", "%s"
out = []
for d in sorted(glob.glob(base + "/" + pat)):
    tag = os.path.basename(d)
    med = []
    for f in sorted(glob.glob(d + "/rep*_c*.csv")):
        v = []
        with open(f) as fh:
            rd = csv.reader(fh); next(rd, None)
            for row in rd:
                try: v.append(int(row[-1]))
                except Exception: pass
        if v:
            v.sort(); med.append(v[len(v)//2])
    if len(med) >= 2:
        out.append((tag, med))
for tag, med in out:
    print(tag, " ".join(str(m) for m in med), max(med) - min(med))
'''


def main(host, remote_dir, pat="*"):
    r = subprocess.run(
        ["ssh", "-o", "ControlMaster=no", "-o", "ControlPath=none", "-n", host,
         "python3 - <<'EOF'\n" + REMOTE % (remote_dir, pat) + "\nEOF"],
        capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr.strip() or "ssh failed")
    print(f"{'arm-run':<22}{'per-receiver p50 (ns)':<28}{'skew (ns)':>10}")
    rows = []
    for line in r.stdout.strip().splitlines():
        parts = line.split()
        print(f"{parts[0]:<22}{' '.join(parts[1:-1]):<28}{parts[-1]:>10}")
        m = re.match(r"ab_b(\d+)_(\w+)$", parts[0])
        if m and len(parts) >= 4:
            rows.append([m.group(1), m.group(2)] + parts[1:])
    if len(sys.argv) > 4 and rows:
        n = max(len(r) for r in rows) - 3   # block, arm, ..., skew
        with open(sys.argv[4], "w") as fh:
            fh.write("block,arm," + ",".join(f"c{i}_p50" for i in range(n)) + ",skew\n")
            for t in rows:
                fh.write(",".join(t) + "\n")
        print(f"\nwrote {sys.argv[4]}")


if __name__ == "__main__":
    main(*sys.argv[1:4])
