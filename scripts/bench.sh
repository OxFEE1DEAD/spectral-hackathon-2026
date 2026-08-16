#!/usr/bin/env bash
# Steady-state benchmark runner for the fan-out transport.
#
# The methodology matters as much as the numbers, so it is worth being explicit
# about what this script does and why:
#
#   0. Fan-out is modelled as N *independent receivers*, each with its own socket,
#      its own shared-memory ring and its own consumer. The sender transmits a
#      separate datagram to every receiver, which is the cost that actually grows
#      with receiver count. For convenience the receivers may share one host --
#      that avoids provisioning N machines, at the cost noted in the write-up
#      (one NIC then carries N copies, which N separate hosts would not).
#   1. The stream runs continuously. The producer and sender are started once with
#      no message limit and left running for the whole session.
#   2. Consumers attach at the producer's live edge (--from-edge) *after* a warm-up
#      period, so no measurement includes process startup, cold caches, or the
#      first-touch page faults of the shared-memory ring.
#   3. Each configuration is measured several times (--reps). A single run cannot
#      establish a p99.9, let alone a p99.99: one unlucky stall moves it by orders
#      of magnitude. Percentiles are reported as a median across repetitions with
#      the spread shown.
#   4. Nothing is written to a file while a measurement is running. Samples go into
#      preallocated memory -- reserved and page-touched before the first sample --
#      and reach the filesystem in one pass after the run. An earlier version wrote
#      one CSV line per message from inside the measurement loop, which put a
#      write() on the timing path: ~1 ms at p99.99 on disk, and unpredictable
#      stalls even on tmpfs.
#   5. The first --drop samples of every run are discarded in analysis, covering
#      the consumer's first lap of the ring while it faults in its own page-table
#      entries.
#
# Three roles, so one script covers a single host and a two-host split:
#
#   --role all   everything here (loopback or a local NIC address)
#   --role send  producer + sender only; runs until interrupted
#   --role recv  receiver + the consumer measurement loop
#
# Run --role recv first, then --role send on the other host.
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

role=all
rate=200000
receivers=1
reps=3
samples=500000
warmup_ms=2000
drop=16384
type=mixed
peers=()
port=51000
bind_addr=""
datagram=1472
src_slots=1024
out_slots=8192
producer_core=1
sender_core=2
receiver_cores="3 5 7"
consumer_cores="4 6 8"
src_shm=/fanout_ring
out_shm=/fanout_out
out_dir=""
tag=""
skip_core_check=0
recv_spin=0
recv_busy_poll=""
send_method=""
no_duplicate=0
stage_capture=0

die() { echo "error: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --role) role="$2"; shift 2 ;;
    --rate) rate="$2"; shift 2 ;;
    --receivers) receivers="$2"; shift 2 ;;
    --consumers) receivers="$2"; shift 2 ;;   # retained as an alias
    --reps) reps="$2"; shift 2 ;;
    --samples) samples="$2"; shift 2 ;;
    --warmup-ms) warmup_ms="$2"; shift 2 ;;
    --drop) drop="$2"; shift 2 ;;
    --type) type="$2"; shift 2 ;;
    --peer) peers+=("$2"); shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --bind) bind_addr="$2"; shift 2 ;;
    --datagram) datagram="$2"; shift 2 ;;
    --src-slots) src_slots="$2"; shift 2 ;;
    --out-slots) out_slots="$2"; shift 2 ;;
    --producer-core) producer_core="$2"; shift 2 ;;
    --sender-core) sender_core="$2"; shift 2 ;;
    --receiver-cores) receiver_cores="$2"; shift 2 ;;
    --consumer-cores) consumer_cores="$2"; shift 2 ;;
    --src-shm) src_shm="$2"; shift 2 ;;
    --out-shm) out_shm="$2"; shift 2 ;;
    --out-dir) out_dir="$2"; shift 2 ;;
    --tag) tag="$2"; shift 2 ;;
    --skip-core-check) skip_core_check=1; shift ;;
    --spin) recv_spin=1; shift ;;   # comparison only; busy-poll is the shipping mode
    --busy-poll) recv_busy_poll="$2"; shift 2 ;;  # force it, for the same comparison
    --send-method) send_method="$2"; shift 2 ;;   # auto|sendmmsg|sendto|connected
    --no-duplicate) no_duplicate=1; shift ;;      # isolate fan-out cost from redundancy
    --stages) stage_capture=1; shift ;;           # capture per-stage timings on receiver 0
    -h|--help) sed -n '2,36p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ "$role" =~ ^(all|send|recv)$ ]] || die "--role must be all, send or recv"
read -ra ccores <<< "$consumer_cores"
read -ra rcores <<< "$receiver_cores"
if [[ "$role" != send ]]; then
  [[ ${#ccores[@]} -ge $receivers ]] || die "need $receivers consumer cores (--consumer-cores)"
  [[ ${#rcores[@]} -ge $receivers ]] || die "need $receivers receiver cores (--receiver-cores)"
fi
if [[ "$role" == send && ${#peers[@]} -eq 0 ]]; then die "--peer is required"; fi
if [[ "$role" == all && ${#peers[@]} -eq 0 ]]; then peers=(127.0.0.1); fi

# One destination per receiver. Given a bare host, expand it across the port range
# the receivers bind, so the two sides agree without the operator listing each one.
if [[ ${#peers[@]} -eq 1 && $receivers -gt 1 && "${peers[0]}" != *:* ]]; then
  base_peer="${peers[0]}"; peers=()
  for ((i = 0; i < receivers; ++i)); do peers+=("$base_peer:$((port + i))"); done
fi

tag="${tag:-r${rate}_n${receivers}}"
out_dir="${out_dir:-$root/data/$tag}"

# Preflight: a core that is isolated but shared with another busy thread produces
# latency quantised to the scheduler timeslice, which is indistinguishable from a
# network problem in the results. Refusing to measure is better than publishing it.
if [[ $skip_core_check -eq 0 ]]; then
  case "$role" in
    all)  check_cores=("$producer_core" "$sender_core" "${rcores[@]:0:$receivers}" "${ccores[@]:0:$receivers}") ;;
    send) check_cores=("$producer_core" "$sender_core") ;;
    recv) check_cores=("${rcores[@]:0:$receivers}" "${ccores[@]:0:$receivers}") ;;
  esac
  echo "--- preflight: core check ---" >&2
  "$here/check_cores.sh" --secs 2 "${check_cores[@]}" >&2 || \
    die "core check failed; fix the cores or pass --skip-core-check to override"
fi

pids=()
stage_files=()
cleanup() {
  for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  for f in "${stage_files[@]:-}"; do rm -f "$f" 2>/dev/null || true; done
}
trap cleanup EXIT

# One receiver process per receiver: its own port, its own ring, its own core. This
# is what makes the sender replicate rather than broadcast.
start_receivers() {
  local i
  for ((i = 0; i < receivers; ++i)); do
    rm -f "/dev/shm${out_shm}${i}" 2>/dev/null || true
    taskset -c "${rcores[$i]}" "$root/transport/bin/receiver" \
      --shm "${out_shm}${i}" --slots "$out_slots" --port "$((port + i))" \
      ${bind_addr:+--bind "$bind_addr"} --core "${rcores[$i]}" --idle-ms 0 \
      $( [[ $recv_spin -eq 1 ]] && echo --spin ) \
      ${recv_busy_poll:+--busy-poll "$recv_busy_poll"} \
      $( [[ $stage_capture -eq 1 && $i -eq 0 ]] && echo --stage-csv "$out_dir/stages.csv" ) &
    pids+=($!)
  done
  sleep 0.5
}

start_stream() {
  rm -f "/dev/shm${src_shm}" 2>/dev/null || true
  taskset -c "$producer_core" "$root/harness/bin/producer" \
    --shm "$src_shm" --slots "$src_slots" --count 0 --rate "$rate" --type "$type" \
    2>/dev/null &
  pids+=($!)
  sleep 0.3
  local peer_args=() p
  for p in "${peers[@]}"; do peer_args+=(--peer "$p"); done
  taskset -c "$sender_core" "$root/transport/bin/sender" \
    --shm "$src_shm" --slots "$src_slots" "${peer_args[@]}" --port "$port" \
    --datagram "$datagram" --core "$sender_core" --count 0 --idle-ms 0 \
    ${send_method:+--send-method "$send_method"} \
    $( [[ $no_duplicate -eq 1 ]] && echo --no-duplicate ) &
  pids+=($!)
}

# One repetition: attach `consumers` consumers at the live edge, let each collect
# `samples` messages, then tear them down. The stream keeps running throughout.
run_rep() {
  local rep="$1"
  local cpids=() i stage
  # One consumer per receiver, each reading only its own receiver's ring, so the
  # per-file results are per-receiver and the skew between them is measurable.
  for ((i = 0; i < receivers; ++i)); do
    stage="/dev/shm/bench_${tag}_rep${rep}_c${i}.$$.csv"
    stage_files+=("$stage")
    taskset -c "${ccores[$i]}" "$root/harness/bin/consumer" \
      --shm "${out_shm}${i}" --slots "$out_slots" --from-edge \
      --count "$samples" --csv "$stage" --idle-ms 30000 >/dev/null 2>&1 &
    cpids+=($!)
  done
  local rc=0
  for p in "${cpids[@]}"; do wait "$p" || rc=1; done
  for ((i = 0; i < receivers; ++i)); do
    stage="/dev/shm/bench_${tag}_rep${rep}_c${i}.$$.csv"
    [[ -f "$stage" ]] && mv "$stage" "$out_dir/rep${rep}_c${i}.csv"
  done
  return $rc
}

case "$role" in
  send)
    start_stream
    echo "sender side running at ${rate} msg/s; Ctrl-C when the receive side is done" >&2
    wait
    ;;
  all|recv)
    mkdir -p "$out_dir"
    start_receivers
    if [[ "$role" == all ]]; then
      start_stream
    else
      echo "$receivers receiver(s) up on ports $port..$((port + receivers - 1)); start the sender host now" >&2
    fi
    echo "warming up for ${warmup_ms} ms before the first measurement" >&2
    sleep "$(awk -v m="$warmup_ms" 'BEGIN {print m/1000}')"
    for ((r = 1; r <= reps; ++r)); do
      echo "rep $r/$reps: $receivers receiver(s) x $samples samples" >&2
      run_rep "$r" || echo "  warning: a consumer exited non-zero in rep $r" >&2
    done
    echo "samples written to $out_dir" >&2
    echo "summarise with: scripts/summarize.py --drop $drop $out_dir" >&2
    ;;
esac
