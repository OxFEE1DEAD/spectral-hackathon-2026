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
#      and reach the filesystem in one pass after the run. Writing one CSV line per
#      message from inside the measurement loop would put a write() on the timing
#      path: ~1 ms at p99.99 on disk, and unpredictable stalls even on tmpfs.
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
# Which mechanism moves datagrams. Both ends are kernel UDP sockets either way;
# iouring drives them through a submission ring instead of send()/recv().
backend=udp
sqpoll=0
sq_core=""
iou_poll=""
recv_buffers=""
xdp_iface=""
xdp_queue=0
# Source address for the sender's sockets. Needed on any path where the outgoing uplink is
# selected by an `ip rule` per local address rather than by destination prefix: an unbound
# socket then leaves by the default route, which is the management interface, not the link
# under test.
src_addr=""
# The sender exits on its own after this long with nothing to relay. It never triggers
# during a run, because the producer streams continuously for the whole sweep. It exists
# so that an orphan cannot outlive its harness: when an ssh session carrying a run died,
# a root-owned AF_XDP sender kept transmitting -- and holding a bound transmit queue --
# until it was found and killed by hand.
sender_idle_ms=60000
# Ask the receiver for kernel receive timestamps, splitting the wire leg at its kernel
# boundary. This perturbs the path it measures -- recv() becomes recvmsg() with control
# data -- so it is for decomposition runs only, never for a headline latency figure.
rx_tstamp=0
# Depth of the receiver's preallocated per-stage buffer, in messages. The default in the
# receiver is sized for a 200k msg/s run; at 1-2M msg/s a run produces several times that
# and the stage capture would silently stop early, so the sweep sets it explicitly.
stage_capacity=""
# Measurement-only sender flag: kernel transmit timestamps, to separate the sending
# kernel's protocol path from the opaque leg beyond it.
tx_tstamp=0
# Receiver architecture: one thread polling, another publishing. See poll_split.h.
split_poll=0
publish_core=""
queue_slots=""
# Send every datagram twice from distinct source ports, first copy to arrive wins.
dual_path=0
# The redundant leg: the same four-tuple diversity, but written only into idle time and
# optionally over a different submission mechanism, so it shares no transmit queue with
# the primary. --dual-path's tail benefit without --dual-path's median cost.
dup_path=0
dup_backend=udp
dup_port=""
dup_sqpoll=0
dup_sq_core=""
dup_xdp_queue=""
dup_src_port=""

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
    --backend) backend="$2"; shift 2 ;;           # udp|iouring
    --sqpoll) sqpoll=1; shift ;;                  # iouring: kernel submission thread
    --sq-core) sq_core="$2"; shift 2 ;;           # and the core it runs on
    --iou-poll) iou_poll="$2"; shift 2 ;;         # iouring: auto|napi|polled
    --recv-buffers) recv_buffers="$2"; shift 2 ;; # iouring: provided-buffer pool depth
    --src-addr) src_addr="$2"; shift 2 ;;         # sender source address (policy-routed paths)
    --xdp-iface) xdp_iface="$2"; shift 2 ;;       # afxdp: interface to transmit through
    --xdp-queue) xdp_queue="$2"; shift 2 ;;       # afxdp: transmit queue to bind
    --sender-idle-ms) sender_idle_ms="$2"; shift 2 ;;  # orphan self-destruct window
    --rx-tstamp) rx_tstamp=1; shift ;;            # split the wire leg at the receiver
    --stage-capacity) stage_capacity="$2"; shift 2 ;;  # per-stage buffer depth
    --tx-tstamp) tx_tstamp=1; shift ;;            # split the sending kernel off the wire leg
    --split-poll) split_poll=1; shift ;;          # dedicated polling thread on the receiver
    --publish-core) publish_core="$2"; shift 2 ;; # core for the publishing thread
    --queue-slots) queue_slots="$2"; shift 2 ;;   # handoff queue depth
    --dual-path) dual_path=1; shift ;;            # two source ports, first copy wins
    --dup-path) dup_path=1; shift ;;              # redundant leg, idle time only
    --dup-backend) dup_backend="$2"; shift 2 ;;   # udp|iouring|xdp for that leg
    --dup-port) dup_port="$2"; shift 2 ;;         # its destination port
    --dup-sqpoll) dup_sqpoll=1; shift ;;          # io_uring leg: kernel submission thread
    --dup-sq-core) dup_sq_core="$2"; shift 2 ;;
    --dup-xdp-queue) dup_xdp_queue="$2"; shift 2 ;;
    --dup-src-port) dup_src_port="$2"; shift 2 ;;
    -h|--help) sed -n '2,36p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ "$role" =~ ^(all|send|recv)$ ]] || die "--role must be all, send or recv"
[[ "$backend" =~ ^(udp|iouring|xdp)$ ]] || die "--backend must be udp, iouring or xdp"
if [[ $split_poll -eq 1 && -z "$publish_core" ]]; then
  die "--split-poll needs --publish-core: the publishing thread must be pinned to a core the preflight has checked"
fi
# AF_XDP can now be either end of the send path: the primary, or the redundant leg with a
# kernel-UDP primary. The interface and the privilege are needed in both cases, so the
# check is on "is AF_XDP in use anywhere" rather than on the primary backend alone.
needs_xdp=0
[[ "$backend" == xdp ]] && needs_xdp=1
[[ $dup_path -eq 1 && "$dup_backend" == xdp ]] && needs_xdp=1
if [[ $needs_xdp -eq 1 ]]; then
  [[ -n "$xdp_iface" ]] || die "AF_XDP (--backend xdp or --dup-backend xdp) requires --xdp-iface"
  # AF_XDP needs CAP_NET_RAW. Checked here rather than letting the sender fail later,
  # because a mid-run failure looks like a transport problem.
  sudo -n true 2>/dev/null || die "AF_XDP needs root for the sender (sudo -n failed)"
elif [[ -n "$xdp_iface" ]]; then
  die "--xdp-iface applies only to AF_XDP (--backend xdp or --dup-backend xdp)"
fi

# Redundant-leg guardrails. Every one of these is a combination that would still run and
# still produce plausible latency while measuring something other than what was asked for.
if [[ $dup_path -eq 1 ]]; then
  [[ $dual_path -eq 0 ]] || \
    die "--dual-path and --dup-path are alternatives: the first sends both copies inline, the second only into idle time"
  [[ "$dup_backend" =~ ^(udp|iouring|xdp)$ ]] || die "--dup-backend must be udp, iouring or xdp"
  [[ $no_duplicate -eq 0 ]] || \
    die "--no-duplicate disables the slot --dup-path sends from; drop one of them"
  if [[ "$dup_backend" != iouring ]]; then
    [[ $dup_sqpoll -eq 0 && -z "$dup_sq_core" ]] || \
      die "--dup-sqpoll/--dup-sq-core require --dup-backend iouring"
  fi
  if [[ "$dup_backend" != xdp ]]; then
    [[ -z "$dup_xdp_queue" && -z "$dup_src_port" ]] || \
      die "--dup-xdp-queue/--dup-src-port require --dup-backend xdp"
  elif [[ "$backend" == xdp && -n "$dup_xdp_queue" && "$dup_xdp_queue" == "$xdp_queue" ]]; then
    die "--dup-xdp-queue must differ from --xdp-queue, or the redundant leg shares the queue whose sharing it exists to avoid"
  fi
else
  [[ $dup_sqpoll -eq 0 && -z "$dup_port" && -z "$dup_sq_core" && -z "$dup_xdp_queue" && -z "$dup_src_port" ]] || \
    die "--dup-* flags require --dup-path"
  [[ "$dup_backend" == udp ]] || die "--dup-backend requires --dup-path"
fi
if [[ "$backend" != iouring ]]; then
  [[ $sqpoll -eq 0 && -z "$sq_core" && -z "$iou_poll" && -z "$recv_buffers" ]] || \
    die "--sqpoll/--sq-core/--iou-poll require --backend iouring"
else
  [[ -n "$iou_poll" ]] && [[ "$iou_poll" =~ ^(auto|napi|polled)$ ]] || [[ -z "$iou_poll" ]] || \
    die "--iou-poll must be auto, napi or polled"
  # The UDP-only receive knobs would be silently ignored, which would make a
  # mistyped comparison look like a real one.
  [[ $recv_spin -eq 0 && -z "$recv_busy_poll" ]] || \
    die "--spin/--busy-poll are UDP-only; use --iou-poll with --backend iouring"
  [[ -z "$send_method" ]] || \
    die "--send-method is UDP-only; the io_uring backend always uses connected sockets"
fi
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
  # The split-poll publishing thread spins on its own core, so it belongs in the preflight
  # like every other pinned thread: a busy core there would be measured as transport latency.
  if [[ $split_poll -eq 1 && -n "$publish_core" && "$role" != send ]]; then
    check_cores+=("$publish_core")
  fi
  # Same reasoning for the redundant leg's io_uring submission thread: SQPOLL spins, so its
  # core has to be as clean as the sender's or the leg competes with what it is helping.
  if [[ $dup_path -eq 1 && -n "$dup_sq_core" && "$role" != recv ]]; then
    check_cores+=("$dup_sq_core")
  fi
  # Refuse to start on top of a previous run's processes.
  #
  # This is not hypothetical tidiness. A stale sender left behind by a teardown that did not
  # take relays the same source ring as the new one, so every message goes out twice from two
  # independent sequence streams; the receiving gate then suppresses about half of what
  # arrives and the run looks superficially fine. It cost two configurations before the
  # suppression counter gave it away.
  stale=$(pgrep -x "sender|receiver|producer|consumer" 2>/dev/null | wc -l)
  if [[ "$stale" -gt 0 ]]; then
    echo "--- preflight: stale processes ---" >&2
    pgrep -a -x "sender|receiver|producer|consumer" >&2
    die "$stale relay process(es) already running; a previous run was not torn down"
  fi

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
      --backend "$backend" \
      $( [[ $recv_spin -eq 1 ]] && echo --spin ) \
      ${recv_busy_poll:+--busy-poll "$recv_busy_poll"} \
      ${iou_poll:+--iou-poll "$iou_poll"} \
      ${recv_buffers:+--recv-buffers "$recv_buffers"} \
      $( [[ $rx_tstamp -eq 1 ]] && echo --rx-tstamp ) \
      $( [[ $split_poll -eq 1 ]] && echo --split-poll ) \
      ${publish_core:+--publish-core "$publish_core"} \
      ${queue_slots:+--queue-slots "$queue_slots"} \
      $( [[ $stage_capture -eq 1 && $i -eq 0 ]] && echo --stage-csv "$out_dir/stages.csv" ) \
      $( [[ $stage_capture -eq 1 && $i -eq 0 && -n "$stage_capacity" ]] && echo --stage-capacity "$stage_capacity" ) &
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
  # Only the AF_XDP path is privileged; everything else runs as the invoking user so a
  # backend comparison is not also a privilege comparison any more than it has to be.
  local sudo_prefix=()
  # AF_XDP needs the privilege wherever it is used, primary path or redundant leg.
  [[ $needs_xdp -eq 1 ]] && sudo_prefix=(sudo -n)
  "${sudo_prefix[@]}" taskset -c "$sender_core" "$root/transport/bin/sender" \
    --shm "$src_shm" --slots "$src_slots" "${peer_args[@]}" --port "$port" \
    --datagram "$datagram" --core "$sender_core" --count 0 --idle-ms "$sender_idle_ms" \
    --backend "$backend" \
    $( [[ $tx_tstamp -eq 1 ]] && echo --tx-tstamp ) \
    $( [[ $dual_path -eq 1 ]] && echo --dual-path ) \
    $( [[ $dup_path -eq 1 ]] && echo --dup-path --dup-backend "$dup_backend" ) \
    ${dup_port:+--dup-port "$dup_port"} \
    $( [[ $dup_sqpoll -eq 1 ]] && echo --dup-sqpoll ) \
    ${dup_sq_core:+--dup-sq-core "$dup_sq_core"} \
    ${dup_xdp_queue:+--dup-xdp-queue "$dup_xdp_queue"} \
    ${dup_src_port:+--dup-src-port "$dup_src_port"} \
    ${xdp_iface:+--xdp-iface "$xdp_iface" --xdp-queue "$xdp_queue"} \
    ${src_addr:+--src-addr "$src_addr"} \
    ${send_method:+--send-method "$send_method"} \
    $( [[ $sqpoll -eq 1 ]] && echo --sqpoll ) \
    ${sq_core:+--sq-core "$sq_core"} \
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
