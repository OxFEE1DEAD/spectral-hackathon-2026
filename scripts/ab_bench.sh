#!/usr/bin/env bash
# Counterbalanced A/B runner: measures two or more configurations against each other in a
# way that survives a drifting clock and a drifting environment.
#
# WHY THIS EXISTS
#
# Running configuration A to completion and then configuration B produces numbers that
# cannot be compared, and this was learned the hard way -- two published results had to be
# withdrawn because of it.
#
#   1. The two hosts' clocks drift relative to each other by microseconds over an hour.
#      Every cross-host figure (end-to-end, and the wire leg) is a difference of two
#      clocks, so a configuration measured fifteen minutes after its baseline carries an
#      unknown offset of the same size as the effect being looked for. A median difference
#      of +3.3 us in one session read as -1.2 us in the next.
#
#   2. The environment's own tail is not reproducible. Three runs of an *identical*
#      configuration produced p99.99 values spanning 14.5x. A single baseline-versus-
#      treatment pairing therefore tells you which window you sampled, not which
#      configuration is better.
#
#   3. Repetitions inside one window do not measure either of those. The spread across
#      reps of one run is within-window variation only; it is blind to both the drift
#      between windows and the environment's between-window mood. Quoting it as the
#      uncertainty is what made both withdrawn claims look solid.
#
# WHAT THIS DOES INSTEAD
#
#   * Interleaves. Each arm is measured once per *block*, and blocks repeat. Two arms in
#     one block are minutes apart rather than tens of minutes, so the drift between them
#     is small.
#   * Counterbalances. The order of arms reverses on alternate blocks (A,B then B,A), which
#     cancels the linear component of any drift across a pair of blocks exactly.
#   * Repeats the whole comparison. N blocks give N independent estimates of the *difference*
#     between arms. The spread of those differences is an uncertainty that includes drift and
#     environment, which is the only kind worth quoting.
#   * Brackets each block with an idle clock-offset probe, when --offset-probe is given, so a
#     cross-host figure can be corrected with an offset measured in its own window. The probe
#     never runs concurrently with a load: a one-way load breaks the path symmetry that
#     round-trip halving depends on.
#   * Records provenance. Every run writes a manifest with the commit, the exact flags, the
#     kernel, the core check and the clock state, so a number can be traced back later.
#   * Refuses to guess. A block whose arm produced fewer samples than asked for, or which
#     tripped a validity check, is written to the results file marked invalid rather than
#     silently averaged in.
#   * Times itself. The whole argument for blocking is that two arms in a block are close
#     together in time, and that is not guaranteed -- a flaky control link can stretch a
#     90-second measurement into eleven minutes of ssh retries, at which point the block is
#     no better than running the arms sequentially. Each arm-run records the epoch second it
#     finished, and paired_stats.py reports the within-block gap and complains when it is
#     large. An assumption the design depends on should be measured, not hoped for.
#
# Nothing about any particular machine is baked in here; every host, address and core is an
# argument, exactly as in bench.sh.
#
# Usage:
#   ab_bench.sh --send-host HOST --recv-host HOST --recv-ip ADDR
#               --arm 'NAME | SENDER FLAGS | RECEIVER FLAGS'   (repeat, 2 or more)
#               [--blocks N] [--rate R] [--samples N] [--out DIR] [--offset-probe] ...
#
# Example (two arms, redundancy off against on, six blocks):
#   ab_bench.sh --send-host sender.example --recv-host recv.example --recv-ip 10.0.0.2 \
#     --arm 'off | --no-duplicate       | --rx-tstamp' \
#     --arm 'on  | --dup-path           | --rx-tstamp' \
#     --blocks 6 --rate 200000 --samples 2000000 --out data/ab_redundancy
set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

send_host=""
recv_host=""
recv_ip=""
remote_dir=/tmp/fanout-bench
port=51000
rate=200000
samples=2000000
blocks=6
warmup_ms=8000
drop=16384
datagram=1472
src_slots=4096
out_slots=65536
producer_core=2
sender_core=4
receiver_core=1
consumer_core=11
# Ours: see the fan-out section of SOLUTION.md. Fan-out was hard-wired to one receiver
# here, which makes the axis the task judges most explicitly unmeasurable with this
# harness. The core arguments take a space-separated list when this is above one.
receivers=1
stage_capacity=25000000
out_dir=""
offset_probe=0
probe_send_core=8
probe_recv_core=6
probe_port=51902
# Source addresses, for policy-routed paths. See bench.sh --src-addr.
src_addr=""
probe_src_addr=""
probe_count=40000
probe_rate=5000
# Retry budget. Deliberately modest: the point is to survive a dropped connection, not to
# outlast an outage. Six retries at a 40-second timeout is nearly five minutes for a single
# status query, which silently converts a block design back into a sequential one.
ssh_timeout=20
ssh_retries=3
# How long to wait for a launched runner to show its first worker.
sender_head_start_s=25
run_timeout=900
arm_names=()
arm_send=()
arm_recv=()

die() { echo "error: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
  case "$1" in
    --send-host) send_host="$2"; shift 2 ;;
    --recv-host) recv_host="$2"; shift 2 ;;
    --recv-ip) recv_ip="$2"; shift 2 ;;
    --remote-dir) remote_dir="$2"; shift 2 ;;
    --port) port="$2"; shift 2 ;;
    --rate) rate="$2"; shift 2 ;;
    --samples) samples="$2"; shift 2 ;;
    --blocks) blocks="$2"; shift 2 ;;
    --warmup-ms) warmup_ms="$2"; shift 2 ;;
    --drop) drop="$2"; shift 2 ;;
    --datagram) datagram="$2"; shift 2 ;;
    --src-slots) src_slots="$2"; shift 2 ;;
    --out-slots) out_slots="$2"; shift 2 ;;
    --producer-core) producer_core="$2"; shift 2 ;;
    --sender-core) sender_core="$2"; shift 2 ;;
    --receiver-core) receiver_core="$2"; shift 2 ;;
    --receivers) receivers="$2"; shift 2 ;;
    --consumer-core) consumer_core="$2"; shift 2 ;;
    --stage-capacity) stage_capacity="$2"; shift 2 ;;
    --out) out_dir="$2"; shift 2 ;;
    --offset-probe) offset_probe=1; shift ;;
    --probe-send-core) probe_send_core="$2"; shift 2 ;;
    --probe-recv-core) probe_recv_core="$2"; shift 2 ;;
    --probe-port) probe_port="$2"; shift 2 ;;
    --src-addr) src_addr="$2"; shift 2 ;;
    --probe-src-addr) probe_src_addr="$2"; shift 2 ;;
    --run-timeout) run_timeout="$2"; shift 2 ;;
    --ssh-timeout) ssh_timeout="$2"; shift 2 ;;
    --ssh-retries) ssh_retries="$2"; shift 2 ;;
    --arm)
      # 'NAME | SENDER FLAGS | RECEIVER FLAGS' -- pipe-separated so flags can contain spaces.
      IFS='|' read -r _n _s _r <<< "$2"
      _n="$(echo "$_n" | xargs)" || true
      [[ -n "$_n" ]] || die "--arm needs a name: 'NAME | SENDER FLAGS | RECEIVER FLAGS'"
      arm_names+=("$_n")
      arm_send+=("$(echo "${_s:-}" | xargs || true)")
      arm_recv+=("$(echo "${_r:-}" | xargs || true)")
      shift 2 ;;
    -h|--help) sed -n '2,70p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[[ -n "$send_host" ]] || die "--send-host is required"
[[ -n "$recv_host" ]] || die "--recv-host is required"
[[ -n "$recv_ip" ]] || die "--recv-ip is required"
[[ ${#arm_names[@]} -ge 2 ]] || die "at least two --arm values are needed; this measures differences"
[[ -n "$out_dir" ]] || die "--out is required"
[[ "$blocks" -ge 2 ]] || die "--blocks must be at least 2: one block cannot estimate its own uncertainty"

mkdir -p "$out_dir"
results="$out_dir/blocks.csv"
manifest="$out_dir/manifest.txt"

# ---- remote plumbing --------------------------------------------------------------------
# Every remote call retries: the link to a benchmark host drops often enough that a single
# failed ssh must not be read as a failed measurement.
sh_() { local h="$1"; shift; local o t
  for ((t=1; t<=ssh_retries; t++)); do
    if o=$(timeout "$ssh_timeout" ssh -o BatchMode=yes -o ConnectTimeout=8 "$h" "$@" 2>&1); then
      printf '%s\n' "$o"; return 0; fi
    sleep 4
  done
  echo SSH_UNREACHABLE; return 1; }

wait_link() { local w=0
  while [[ $w -lt 14400 ]]; do
    if timeout 20 ssh -o BatchMode=yes -o ConnectTimeout=8 "$send_host" 'exit 0' 2>/dev/null \
       && timeout 20 ssh -o BatchMode=yes -o ConnectTimeout=8 "$recv_host" 'exit 0' 2>/dev/null; then
      return 0; fi
    sleep 20; w=$((w+20))
  done
  return 1; }

# Launch at most once, deciding from the host's own process list rather than from a marker
# file we may have failed to read.
#
# A marker-based version of this treated an unreadable marker as "did not launch" and
# launched again. On a flaky link that is exactly wrong: two runners start, the second finds
# the first's producer and dies at its stale-process preflight, and the configuration
# silently never runs. An unknown answer must mean wait and ask again, never launch.
# Launch exactly once, then verify by polling until the workers appear.
#
# Two versions of this got it wrong, in opposite directions, and both produced silently
# invalid measurements:
#
#   * A marker-file version treated an unreadable marker as "did not launch" and launched
#     again, so a dropped connection started a second runner.
#   * A process-list version launched, waited six seconds and asked again -- but bench.sh
#     runs a core check and a stale-process preflight before it spawns its first worker, so
#     six seconds is reliably too short. It saw zero, launched again, and the second runner
#     died on the first one's processes.
#
# The rule that works: launch at most once per call, then wait for evidence, and if the
# evidence never arrives say so rather than trying again. "I could not tell" must never mean
# "launch".
launch_once() {  # host command procpattern -> 0 launched, 1 refused to launch
  local h="$1" cmd="$2" pat="$3" n w
  # Refuse to start on top of anything already running, retrying only the *question*. An
  # unknown answer must never mean "launch": a marker-file version of this treated an
  # unreadable marker as "did not launch" and started a second runner, and two senders on one
  # source ring produce a stream the receiving gate half-suppresses while nothing in the
  # latency output looks wrong.
  n=""
  for ((w=1; w<=4; w++)); do
    n=$(sh_ "$h" "pgrep -c -x \"$pat\" 2>/dev/null; true" | head -1 | tr -dc '0-9')
    [[ -n "$n" ]] && break
    sleep 4
  done
  [[ -z "$n" ]] && return 1
  [[ "$n" != "0" ]] && return 0
  # Launched exactly once, and deliberately not verified by polling for the process. An arm
  # is about sixteen seconds of warm-up and measurement, so a poll can easily arrive after it
  # has finished and conclude the launch failed -- one block was discarded that way despite
  # having received 3,207,957 datagrams. A launch that truly did not take yields published=0,
  # which is already an invalid row, and the runner's log says why.
  #
  # NOT through sh_. sh_ retries, and a retried launch is a second runner -- which is the
  # actual root cause of every double-launch seen here. Fixing the decision logic above was
  # not enough while the transport underneath it still repeated the command: a slow ssh
  # returned non-zero after the remote side had already started, the retry launched again, and
  # the second bench.sh died on the first one's processes and wrote the sentinel. One ssh, one
  # attempt, and an unknown outcome is left to the validity check.
  timeout "$ssh_timeout" ssh -o BatchMode=yes -o ConnectTimeout=8 "$h" \
    "cd $remote_dir && setsid bash -c '$cmd' < /dev/null > /dev/null 2>&1 & echo ok" \
    >/dev/null 2>&1
  return 0; }

# Tear down and report what is left, in one round trip per host.
#
# Interrupt the workers FIRST and give them time to report: the sender only prints its
# statistics on the way out, so killing its process group first loses them. The residual
# count comes back from the same call rather than a follow-up query, because on a slow
# control link every extra round trip is charged to the gap between a block's two arms --
# the quantity the whole design depends on keeping small.
teardown() { local h out n
  residual=0
  for h in "$send_host" "$recv_host"; do
    out=$(sh_ "$h" '
      for n in sender receiver producer consumer clock_probe; do
        sudo -n pkill -INT -x $n 2>/dev/null; pkill -INT -x $n 2>/dev/null; done
      sleep 4
      me=$$; chain=""; p=$me
      while [ -n "$p" ] && [ "$p" != "1" ]; do chain="$chain $p"; p=$(ps -o ppid= -p $p 2>/dev/null | tr -d " "); done
      for pid in $(pgrep -f "scripts/bench[.]sh" 2>/dev/null); do
        case " $chain " in *" $pid "*) continue;; esac
        kill -9 -- -$pid 2>/dev/null || kill -9 $pid 2>/dev/null; done
      for n in sender receiver producer consumer clock_probe; do
        sudo -n pkill -KILL -x $n 2>/dev/null; pkill -KILL -x $n 2>/dev/null; done
      for i in $(seq 40); do pgrep -x "sender|receiver|producer|consumer|clock_probe" >/dev/null 2>&1 || break; sleep 0.5; done
      rm -f /dev/shm/fanout_* 2>/dev/null
      printf "LEFT=%s\\n" "$(pgrep -c -x "sender|receiver|producer|consumer|clock_probe" 2>/dev/null; true)"')
    n=$(printf '%s' "$out" | grep -oE 'LEFT=[0-9]+' | head -1 | grep -oE '[0-9]+$')
    # An unreadable answer counts as dirty. Proceeding on "probably fine" is how a stale
    # sender came to relay a second copy of one source ring unnoticed.
    [[ -z "$n" ]] && n=1
    residual=$(( residual + n ))
  done
  [[ "$residual" == "0" ]]; }

wait_sentinel() { local h="$1" f="$2" max="$3" w=0 r
  while [[ $w -lt $max ]]; do
    r=$(sh_ "$h" "cat $f 2>/dev/null || echo P" | head -1)
    case "$r" in P|SSH_UNREACHABLE|"") sleep 10; w=$((w+10));; *) echo "$r"; return 0;; esac
  done; echo TIMEOUT; return 1; }

# ---- clock offset bracket ---------------------------------------------------------------
# Idle only. Under a one-way load the round trip is asymmetric and halving it is invalid --
# a probe run during a load read the offset as -68,750 ns against -2,783 ns idle.
offset_probe_once() {  # label -> prints offset_ns or empty
  [[ $offset_probe -eq 1 ]] || { echo ""; return 0; }
  local label="$1" listening
  sh_ "$recv_host" "pkill -KILL -x clock_probe 2>/dev/null; sleep 1; cd $remote_dir && nohup setsid taskset -c $probe_recv_core ./tools/bin/clock_probe --reflect --bind $recv_ip --port $probe_port --core $probe_recv_core --idle-s 120 > /tmp/refl_$label.log 2>&1 < /dev/null & echo up" >/dev/null 2>&1
  sleep 3
  listening=$(sh_ "$recv_host" "ss -uln 2>/dev/null | grep -c $probe_port" | head -1 | tr -dc '0-9')
  if [[ "${listening:-0}" == "0" ]]; then
    echo ""; return 0
  fi
  sh_ "$send_host" "rm -f /tmp/.pd_$label; cd $remote_dir && setsid bash -c 'taskset -c $probe_send_core ./tools/bin/clock_probe --probe --peer $recv_ip:$probe_port --count $probe_count --rate $probe_rate --core $probe_send_core ${probe_src_addr:+--src $probe_src_addr} --csv /tmp/off_$label.csv > /tmp/off_$label.log 2>&1; echo \$? > /tmp/.pd_$label' < /dev/null > /dev/null 2>&1 & echo up" >/dev/null 2>&1
  wait_sentinel "$send_host" "/tmp/.pd_$label" 200 >/dev/null
  local off
  # clock_probe prints one unambiguous line for this: "subtract N ns", the median offset.
  # tr keeps only digits and a sign, so a failed remote call cannot leak its error string
  # into a numeric field -- an earlier run reported "offset before block: SSH_UNREACHABLE ns".
  off=$(sh_ "$send_host" "grep -oE 'subtract -?[0-9]+ ns' /tmp/off_$label.log | tail -1 | grep -oE '\-?[0-9]+'" | head -1 | tr -dc '0-9-')
  sh_ "$recv_host" 'pkill -INT -x clock_probe 2>/dev/null; true' >/dev/null 2>&1
  sleep 2
  echo "${off:-}"; }

# ---- one measurement --------------------------------------------------------------------
run_arm() {  # block index tag
  local block="$1" idx="$2" tag="$3"
  local arm_start; arm_start=$(date +%s)
  local xs="${arm_send[$idx]}" xr="${arm_recv[$idx]}"
  local rc

  # A row is written even when the control link never came back, because a block that
  # silently loses an arm is indistinguishable from a block that was never scheduled. During
  # an outage this path fired and block 2 kept only its second arm, leaving a paired
  # comparison with nothing to pair against and no record of why.
  if ! wait_link; then
    echo "$block,${arm_names[$idx]},0,,,,,,,,,,,,,,,,,$(date +%s),$(( $(date +%s) - arm_start )),control-link-unreachable" >> "$results"
    echo "  | block $block ${arm_names[$idx]}: control link unreachable"
    return 1
  fi
  local tries=0
  until teardown; do
    tries=$((tries+1)); [[ $tries -ge 3 ]] && break
  done
  if [[ "$residual" != "0" ]]; then
    echo "$block,${arm_names[$idx]},0,,,,,,,,,,,,,,,,,$(date +%s),$(( $(date +%s) - arm_start )),SKIPPED-hosts-not-clean" >> "$results"
    echo "  | block $block ${arm_names[$idx]}: SKIPPED, hosts not clean"
    return 1
  fi

  # The sender starts FIRST. The receiver's warm-up is counted from the receiver's own
  # start, so if the sender is still in its preflight when that warm-up expires the receiver
  # measures an empty stream. That is not hypothetical: one block recorded 5,682,280
  # datagrams sent and 0 received. Datagrams arriving at a port with no listener are simply
  # dropped, so starting the sender early costs nothing and removes the race.
  sh_ "$send_host" "rm -f $remote_dir/log_s_$tag" >/dev/null 2>&1
  if ! launch_once "$send_host" \
    "./scripts/bench.sh --role send --peer $recv_ip --receivers $receivers --port $port --rate $rate --producer-core $producer_core --sender-core $sender_core --src-slots $src_slots --datagram $datagram ${src_addr:+--src-addr $src_addr} $xs > $remote_dir/log_s_$tag 2>&1" \
    "sender|producer"; then
    echo "$block,${arm_names[$idx]},0,,,,,,,,,,,,,,,,,$(date +%s),$(( $(date +%s) - arm_start )),sender-never-started" >> "$results"
    echo "  | block $block ${arm_names[$idx]}: sender never started"
    teardown; return 1
  fi
  # bench.sh spends this long in its core check and stale-process preflight before the
  # sender exists, and the receiver's warm-up is counted from the receiver's own start.
  sleep "$sender_head_start_s"
  sh_ "$recv_host" "rm -f $remote_dir/.done_$tag $remote_dir/log_$tag; rm -rf $remote_dir/data/$tag" >/dev/null 2>&1
  if ! launch_once "$recv_host" \
    "./scripts/bench.sh --role recv --receivers $receivers --reps 1 --samples $samples --bind $recv_ip --port $port --receiver-cores \" $receiver_core\" --consumer-cores \" $consumer_core\" --out-slots $out_slots --warmup-ms $warmup_ms --stages --stage-capacity $stage_capacity $xr --tag $tag --out-dir $remote_dir/data/$tag > $remote_dir/log_$tag 2>&1; echo \$? > $remote_dir/.done_$tag" \
    "receiver|consumer"; then
    echo "$block,${arm_names[$idx]},0,,,,,,,,,,,,,,,,,$(date +%s),$(( $(date +%s) - arm_start )),receiver-never-started" >> "$results"
    echo "  | block $block ${arm_names[$idx]}: receiver never started"
    teardown; return 1
  fi
  rc=$(wait_sentinel "$recv_host" "$remote_dir/.done_$tag" "$run_timeout")
  teardown

  # Summarise where the data is; only kilobytes come back. A stage file at these rates is
  # hundreds of megabytes and fetching one is the least reliable step in a measurement.
  sh_ "$recv_host" "cd $remote_dir && { echo '#gate'; grep -E 'published|suppressed|datagram reorder|datagram gaps|malformed|first-copy loss|duplicates recvd|arrived first|seq gaps' log_$tag | head -$((12 * receivers));
      echo '#legs'; for c in shm_ns wire_ns publish_ns rx_delivery_ns; do ./scripts/stage_pct.sh data/$tag/stages.csv \$c 2>/dev/null; done;
      echo '#e2e'; python3 ./scripts/summarize.py --drop $drop --json data/$tag 2>/dev/null; }" \
      > "$out_dir/raw_$tag.txt" 2>&1

  python3 "$here/ab_row.py" --raw "$out_dir/raw_$tag.txt" --block "$block" \
      --arm "${arm_names[$idx]}" --rc "$rc" --want-samples "$samples" \
      --finished-at "$(date +%s)" --elapsed-s "$(( $(date +%s) - arm_start ))" >> "$results"
  tail -1 "$results" | sed 's/^/  | /'
}

# ---- provenance -------------------------------------------------------------------------
{
  echo "ab_bench run"
  echo "date_utc         : $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "commit           : $(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "tree_dirty       : $(git -C "$root" status --porcelain 2>/dev/null | wc -l) file(s) modified"
  echo "blocks           : $blocks"
  echo "rate             : $rate"
  echo "samples_per_block: $samples"
  echo "drop             : $drop"
  echo "warmup_ms        : $warmup_ms"
  echo "datagram         : $datagram"
  echo "cores            : producer=$producer_core sender=$sender_core receiver=$receiver_core consumer=$consumer_core"
  echo "offset_probe     : $offset_probe"
  for i in "${!arm_names[@]}"; do
    echo "arm[$i]           : ${arm_names[$i]} | send='${arm_send[$i]}' | recv='${arm_recv[$i]}'"
  done
} > "$manifest"

wait_link || die "neither host reachable"
echo "syncing and building"
for h in "$send_host" "$recv_host"; do
  for t in 1 2 3 4 5; do
    rsync -a --timeout=40 --exclude data --exclude .git --exclude bin --exclude '*.ipynb' \
      "$root/" "$h:$remote_dir/" 2>/dev/null && break
    sleep 5
  done
  # Ours: see SOLUTION.md. `echo BUILD_OK` used to run unconditionally, so a compile
  # error left the previous binaries in place and the whole matrix was measured against
  # stale code that silently ignored the arm's flags. That is not a hypothetical: one
  # three-arm run recorded "no-data" for every block of the arm whose flag was new.
  # A build that does not build is a failed run, not a warning.
  build_log=$(sh_ "$h" "cd $remote_dir && make -C harness all && make -C transport all && make -C tools all" 2>&1)
  if [[ $? -ne 0 ]] || grep -qiE '\berror\b|Error [0-9]' <<< "$build_log"; then
    echo "$build_log" | tail -20 >&2
    die "build failed on $h"
  fi
  echo "  $h: build ok"
done

# Environment provenance, captured once from each host. Kernel release is deliberately
# reduced to major.minor.patch: the exact build is not something this write-up publishes.
{
  echo
  for h in "$send_host" "$recv_host"; do
    echo "--- host $( [[ "$h" == "$send_host" ]] && echo sender || echo receiver ) ---"
    sh_ "$h" 'uname -r | cut -d- -f1 | tr -cd "0-9." | sed "s/^/kernel           : /;s/\$//"
              echo
              chronyc tracking 2>/dev/null | grep -iE "RMS offset|Root dispersion" | sed "s/^/clock            : /"
              nproc --all | sed "s/^/cores_total      : /"
              nproc | sed "s/^/cores_available  : /"'
  done
} >> "$manifest"
echo "manifest written to $manifest"

echo "block,arm,valid,n_samples,published,suppressed,reorder,lost_datagrams,loss_pct,dup_first,undelivered,shm_p50,wire_p50,rx_delivery_p50,rx_delivery_p99,e2e_p50,e2e_p99,e2e_p999,e2e_p9999,finished_at,elapsed_s,note" > "$results"

nA=${#arm_names[@]}
for ((b=1; b<=blocks; b++)); do
  block_start=$(date +%s)
  echo "=== block $b of $blocks ==="
  off_pre=$(offset_probe_once "b${b}pre")
  [[ -n "$off_pre" ]] && echo "  offset before block: $off_pre ns"
  # Counterbalance: reverse the arm order on alternate blocks, so the linear component of
  # any drift across a pair of blocks cancels instead of always favouring the arm that runs
  # first.
  order=()
  if (( b % 2 == 1 )); then
    for ((i=0; i<nA; i++)); do order+=("$i"); done
  else
    for ((i=nA-1; i>=0; i--)); do order+=("$i"); done
  fi
  for i in "${order[@]}"; do
    run_arm "$b" "$i" "ab_b${b}_${arm_names[$i]}"
  done
  off_post=$(offset_probe_once "b${b}post")
  [[ -n "$off_post" ]] && echo "  offset after block : $off_post ns"
  if [[ -n "$off_pre" && -n "$off_post" ]]; then
    echo "$b,$off_pre,$off_post" >> "$out_dir/offsets.csv"
  fi
  echo "  block $b took $(( $(date +%s) - block_start ))s"
done

echo
echo "done: $results"
python3 "$here/paired_stats.py" "$results" || true
