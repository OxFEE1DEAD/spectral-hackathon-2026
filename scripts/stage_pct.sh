#!/usr/bin/env bash
# Percentiles of one column of a stages.csv, computed where the file is.
#
# Written to run on the measurement host rather than pulling the file back. A stage file for
# a high-rate run is a couple of hundred megabytes, and the link to these hosts drops often
# enough that fetching one is the least reliable step in a measurement. The summary is a few
# hundred bytes.
#
# Deliberately sort(1) rather than an in-memory sort: the files run to tens of millions of
# rows and sort spills to disk instead of failing.
#
# The warm-up drop is one percent rather than a fixed count, because the receiver
# preallocates and page-touches its per-stage buffer at startup and the datagrams that queue
# behind that land in the opening rows. A fixed 16k drop was not enough and left a
# millisecond-scale artefact in the tail.
#
# Usage: stage_pct.sh FILE COLUMN_NAME
set -uo pipefail

f="${1:?usage: stage_pct.sh FILE COLUMN}"
col="${2:?usage: stage_pct.sh FILE COLUMN}"
[[ -r "$f" ]] || { echo "$col: unreadable $f"; exit 1; }

idx=$(head -1 "$f" | tr ',' '\n' | grep -nx "$col" | cut -d: -f1)
[[ -n "${idx:-}" ]] || { echo "$col: no such column"; exit 1; }

total=$(( $(wc -l < "$f") - 1 ))
skip=$(( total / 100 ))
[[ $skip -lt 1 ]] && skip=1

# Zero means "not recorded" for the receive-delivery column, so those rows are excluded
# rather than dragging the distribution down.
tail -n +$((skip + 2)) "$f" \
  | cut -d, -f"$idx" \
  | awk 'NF && $1+0 > 0' \
  | sort -n \
  | awk -v c="$col" -v tot="$total" '
      { v[NR] = $1 }
      END {
        if (NR == 0) { printf "%-16s no samples\n", c; exit }
        p50  = v[int(0.50   * NR) + 0 == 0 ? 1 : int(0.50   * NR)]
        p99  = v[int(0.99   * NR) + 0 == 0 ? 1 : int(0.99   * NR)]
        p999 = v[int(0.999  * NR) + 0 == 0 ? 1 : int(0.999  * NR)]
        p9999= v[int(0.9999 * NR) + 0 == 0 ? 1 : int(0.9999 * NR)]
        printf "%-16s n=%d p50=%d p99=%d p99.9=%d p99.99=%d max=%d\n", c, NR, p50, p99, p999, p9999, v[NR]
      }'
