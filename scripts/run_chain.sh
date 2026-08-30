#!/usr/bin/env bash
# Run the producer -> sender -> network -> receiver -> consumer chain.
#
# Three roles, so the same script drives a single-host run and a two-host run:
#
#   --role all   producer + sender + receiver + consumer on this host (loopback
#                or a real NIC address). Both timestamps come from one clock, so
#                the latency figure is a true one-way delivery time.
#   --role send  producer + sender only. Run this on the sending host.
#   --role recv  receiver + consumer only. Run this on a receiving host.
#
# Across two hosts the clocks are independent, so the consumer's latency figure
# includes their offset -- see the write-up for how that is measured and removed.
# Nothing about any particular machine is baked in here: every address and core
# is an argument.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

role=all
rate=200000
count=200000
type=mixed
peers=()
port=51000
bind_addr=""
datagram=1472
src_slots=1024
out_slots=8192
producer_core=1
sender_core=2
receiver_core=3
consumer_core=4
src_shm=/fanout_ring
out_shm=/fanout_out
out_dir="$root/data"
tag=""
busy_poll=0
# Which mechanism carries datagrams: kernel UDP sockets driven by send()/recv(), or the
# same sockets driven through an io_uring submission ring. Applies to both ends.
backend=udp
sqpoll=0
sq_core=""
iou_poll=auto

die() { echo "error: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --role) role="$2"; shift 2 ;;
    --rate) rate="$2"; shift 2 ;;
    --count) count="$2"; shift 2 ;;
    --type) type="$2"; shift 2 ;;
    --peer) peers+=("$2"); shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --bind) bind_addr="$2"; shift 2 ;;
    --datagram) datagram="$2"; shift 2 ;;
    --src-slots) src_slots="$2"; shift 2 ;;
    --out-slots) out_slots="$2"; shift 2 ;;
    --producer-core) producer_core="$2"; shift 2 ;;
    --sender-core) sender_core="$2"; shift 2 ;;
    --receiver-core) receiver_core="$2"; shift 2 ;;
    --consumer-core) consumer_core="$2"; shift 2 ;;
    --src-shm) src_shm="$2"; shift 2 ;;
    --out-shm) out_shm="$2"; shift 2 ;;
    --out-dir) out_dir="$2"; shift 2 ;;
    --tag) tag="$2"; shift 2 ;;
    --busy-poll) busy_poll="$2"; shift 2 ;;
    --backend) backend="$2"; shift 2 ;;
    --sqpoll) sqpoll=1; shift ;;
    --sq-core) sq_core="$2"; shift 2 ;;
    --iou-poll) iou_poll="$2"; shift 2 ;;
    -h|--help) sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ "$role" =~ ^(all|send|recv)$ ]] || die "--role must be all, send or recv"
[[ "$backend" =~ ^(udp|iouring)$ ]] || die "--backend must be udp or iouring"
[[ "$iou_poll" =~ ^(auto|napi|polled)$ ]] || die "--iou-poll must be auto, napi or polled"
if [[ "$backend" == udp && ( "$sqpoll" == 1 || -n "$sq_core" ) ]]; then
  die "--sqpoll/--sq-core require --backend iouring"
fi
if [[ "$role" != recv && ${#peers[@]} -eq 0 ]]; then
  if [[ "$role" == all ]]; then peers=(127.0.0.1); else die "--peer is required"; fi
fi

mkdir -p "$out_dir"
label="${tag:-r${rate}}"
lat_csv="$out_dir/latency_${label}.csv"

# The consumer records samples into preallocated memory and dumps them once, after
# the run, so nothing on the measurement path ever calls write(). Staging that single
# dump on tmpfs as well costs nothing and keeps even the final flush off the disk.
lat_stage="/dev/shm/latency_${label}.$$.csv"

# A stale segment from a killed run would be read as live data.
cleanup_shm() { rm -f "/dev/shm${src_shm}" "/dev/shm${out_shm}" 2>/dev/null || true; }

# Move the staged samples into place; called only after the consumer has exited.
promote_samples() {
  [[ -f "$lat_stage" ]] || return 0
  mv "$lat_stage" "$lat_csv"
  echo "latency samples: $lat_csv" >&2
}
pids=()
cleanup() {
  for p in "${pids[@]:-}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -f "$lat_stage" 2>/dev/null || true
}
trap cleanup EXIT

start_receiver_side() {
  # The receiver creates the output segment, so it must precede the consumer.
  taskset -c "$receiver_core" "$root/transport/bin/receiver" \
    --shm "$out_shm" --slots "$out_slots" --port "$port" \
    ${bind_addr:+--bind "$bind_addr"} --core "$receiver_core" \
    --backend "$backend" \
    $([[ "$backend" == iouring ]] && echo "--iou-poll $iou_poll" || echo "--busy-poll $busy_poll") \
    --idle-ms 3000 &
  pids+=($!)
  sleep 0.3
  taskset -c "$consumer_core" "$root/harness/bin/consumer" \
    --shm "$out_shm" --slots "$out_slots" --count "$count" --csv "$lat_stage" &
  consumer_pid=$!
  pids+=("$consumer_pid")
}

start_sender_side() {
  taskset -c "$producer_core" "$root/harness/bin/producer" \
    --shm "$src_shm" --slots "$src_slots" --count 0 --rate "$rate" --type "$type" &
  producer_pid=$!
  pids+=("$producer_pid")
  sleep 0.2
  local peer_args=()
  local p
  for p in "${peers[@]}"; do peer_args+=(--peer "$p"); done
  taskset -c "$sender_core" "$root/transport/bin/sender" \
    --shm "$src_shm" --slots "$src_slots" \
    "${peer_args[@]}" --port "$port" --datagram "$datagram" \
    --core "$sender_core" --count "$count" --idle-ms 3000 \
    --backend "$backend" \
    $([[ "$sqpoll" == 1 ]] && echo "--sqpoll") \
    ${sq_core:+--sq-core "$sq_core"} &
  sender_pid=$!
  pids+=("$sender_pid")
}

cleanup_shm

case "$role" in
  all)
    start_receiver_side
    start_sender_side
    wait "$consumer_pid" || true
    wait "$sender_pid" 2>/dev/null || true
    kill "$producer_pid" 2>/dev/null || true
    promote_samples
    ;;
  recv)
    start_receiver_side
    echo "receiver side up; start the sender host now" >&2
    wait "$consumer_pid" || true
    promote_samples
    ;;
  send)
    start_sender_side
    wait "$sender_pid" || true
    kill "$producer_pid" 2>/dev/null || true
    ;;
esac
