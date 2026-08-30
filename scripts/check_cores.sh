#!/usr/bin/env bash
# Verify that the cores you are about to pin spinning threads to are actually free.
#
# This exists because getting it wrong is nearly invisible in the results. A core
# that is isolated but shared with another busy-spinning thread produces latency
# quantised to the scheduler timeslice -- 4 ms on a CONFIG_HZ=250 kernel -- which
# looks exactly like a network or queueing problem and is not one.
#
# isolcpus only stops the scheduler from *migrating* work onto a core. Anything
# explicitly pinned there with taskset still runs there, and a load average or a
# ps snapshot will not show you that reliably. Measured utilisation will.
#
# Usage: check_cores.sh 1 2 3 4        # cores intended for spinning threads
#        check_cores.sh --secs 5 32 34
set -uo pipefail

secs=3
cores=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --secs) secs="$2"; shift 2 ;;
    -h|--help) sed -n '2,17p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) cores+=("$1"); shift ;;
  esac
done
if [[ ${#cores[@]} -eq 0 ]]; then
  echo "usage: check_cores.sh [--secs N] CORE [CORE ...]" >&2
  exit 2
fi

# Utilisation is the only reliable signal: sample /proc/stat twice and diff.
snapshot() {
  awk '/^cpu[0-9]/ {idle=$5+$6; tot=0; for (i=2;i<=NF;i++) tot+=$i; print substr($1,4), tot, idle}' \
    /proc/stat
}
before=$(snapshot)
sleep "$secs"
after=$(snapshot)

busy_pct() {
  local core="$1"
  local b a
  b=$(echo "$before" | awk -v c="$core" '$1==c {print $2, $3}')
  a=$(echo "$after"  | awk -v c="$core" '$1==c {print $2, $3}')
  [[ -n "$b" && -n "$a" ]] || { echo "n/a"; return; }
  echo "$b $a" | awk '{dt=$3-$1; di=$4-$2; if (dt<=0) print "n/a"; else printf "%.2f", 100*(dt-di)/dt}'
}

isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "")
nohz=$(cat /sys/devices/system/cpu/nohz_full 2>/dev/null || echo "")

in_list() {  # $1 = core, $2 = kernel range list like "1-6,10"
  local core="$1" list="${2:-}"
  [[ -z "$list" ]] && return 1
  local part lo hi
  IFS=',' read -ra parts <<< "$list"
  for part in "${parts[@]}"; do
    if [[ "$part" == *-* ]]; then
      lo="${part%%-*}"; hi="${part##*-}"
      (( core >= lo && core <= hi )) && return 0
    elif [[ "$part" == "$core" ]]; then
      return 0
    fi
  done
  return 1
}

# Threads pinned to exactly one core, excluding the per-CPU kernel threads that
# exist on every core and are dormant. A threaded IRQ handler here is worth
# knowing about: it will preempt a spinning thread if its device ever fires.
pinned_here() {
  local core="$1" tid comm aff
  for tid in $(ps -eLo tid --no-headers 2>/dev/null); do
    comm=$(cat "/proc/$tid/comm" 2>/dev/null) || continue
    case "$comm" in
      cpuhp/*|migration/*|ksoftirqd/*|kworker/*|idle_inject/*|irq_work/*|rcu*|\
      ksoftirqd|watchdog/*) continue ;;
    esac
    aff=$(taskset -cp "$tid" 2>/dev/null | sed 's/.*: //') || continue
    [[ "$aff" == "$core" ]] && echo "      pinned: $comm (tid $tid)"
  done
}

status=0
printf "%-6s %8s %10s %10s %9s  %s\n" core busy isolated nohz_full sibling verdict
for c in "${cores[@]}"; do
  pct=$(busy_pct "$c")
  iso=no; in_list "$c" "$isolated" && iso=yes
  nh=no;  in_list "$c" "$nohz" && nh=yes
  sib=$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list" 2>/dev/null || echo "?")
  # A sibling list naming another cpu means SMT is on and the partner core shares
  # execution resources -- pinning here gives you half a core, not a core.
  smt_ok=yes; [[ "$sib" == *,* || "$sib" == *-* ]] && smt_ok=no

  verdict="ok"
  if [[ "$pct" == "n/a" ]]; then
    verdict="OFFLINE?"; status=1
  elif awk -v p="$pct" 'BEGIN {exit !(p > 5)}'; then
    verdict="BUSY -- something else is running here"; status=1
  elif [[ "$iso" == no ]]; then
    verdict="not isolated -- tail will include scheduler noise"; status=1
  elif [[ "$smt_ok" == no ]]; then
    verdict="SMT sibling active ($sib)"; status=1
  elif [[ "$nh" == no ]]; then
    verdict="ok, but tick still runs (no nohz_full)"
  fi

  printf "%-6s %7s%% %10s %10s %9s  %s\n" "$c" "$pct" "$iso" "$nh" "$sib" "$verdict"
  pinned_here "$c"
done

echo
if [[ $status -eq 0 ]]; then
  echo "all requested cores look usable for pinned spinning threads"
else
  echo "at least one core is unsuitable -- results measured here will not mean what you think" >&2
fi
exit $status
