#!/usr/bin/env bash
# End-to-end run across the two local namespaces:
#
#   ns tx:  producer -> [/ring_tx] -> sender  ==UDP==>
#   ns rx:                        ==> receiver -> [/ring_rx] -> consumer
#
# Start order is the one documented for the real deployment, and it is not
# arbitrary: the receiver creates the ring the consumer attaches to, and the
# sender must not start publishing before there is something to publish into.
#
#   receiver -> consumer -> sender -> producer
#
# The two rings must have different names. Network namespaces isolate the
# network only; /dev/shm is shared, so a single ring name would have the
# producer and the receiver writing into the same buffer.
#
# Usage: bench/e2e-netns.sh [--count N] [--rate R] [--streams N]
#                           [--drop-pct X] [--netem "1%"] [--out DIR]
set -euo pipefail

COUNT=200000
RATE=200000
STREAMS=1
DROP_PCT=0
NETEM=""
OUT=""
TYPE=mixed

while [ $# -gt 0 ]; do
  case "$1" in
    --count) COUNT="$2"; shift 2;;
    --rate) RATE="$2"; shift 2;;
    --streams) STREAMS="$2"; shift 2;;
    --drop-pct) DROP_PCT="$2"; shift 2;;
    --netem) NETEM="$2"; shift 2;;
    --type) TYPE="$2"; shift 2;;
    --out) OUT="$2"; shift 2;;
    *) echo "unknown arg: $1" >&2; exit 2;;
  esac
done

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
OUT="${OUT:-$REPO/results/e2e-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT"

RX_IP=10.0.0.2
DATA_PORT=9100
CTRL_PORT=9101
SHM_TX=/ring_tx
SHM_RX=/ring_rx
SLOTS=1024

if ! ip netns list 2>/dev/null | grep -q '^tx'; then
  echo "netns not up; run: sudo bench/netns-up.sh" >&2
  exit 1
fi

if [ -n "$NETEM" ]; then
  # shellcheck disable=SC2086
  sudo "$REPO/bench/netns-loss.sh" $NETEM >/dev/null
else
  sudo "$REPO/bench/netns-loss.sh" off >/dev/null
fi

cleanup() {
  sudo pkill -f 'transport/bin/(sender|receiver)' 2>/dev/null || true
  sudo pkill -f 'harness/bin/(producer|consumer)' 2>/dev/null || true
  sudo rm -f "/dev/shm${SHM_TX}" "/dev/shm${SHM_RX}" 2>/dev/null || true
}
trap cleanup EXIT
cleanup

echo "== run: count=$COUNT rate=$RATE streams=$STREAMS drop=${DROP_PCT}% netem='${NETEM:-off}'"
echo "== out: $OUT"

sudo ip netns exec rx "$REPO/transport/bin/receiver" \
  --listen "0.0.0.0:${DATA_PORT}" --shm "$SHM_RX" --slots "$SLOTS" \
  --control-port "$CTRL_PORT" --drop-pct "$DROP_PCT" --idle-ms 3000 \
  >"$OUT/receiver.log" 2>&1 &
sleep 0.4

sudo ip netns exec rx "$REPO/harness/bin/consumer" \
  --shm "$SHM_RX" --slots "$SLOTS" --from-edge --csv "$OUT/latency.csv" \
  --idle-ms 3000 >"$OUT/consumer.log" 2>&1 &
sleep 0.3

sudo ip netns exec tx "$REPO/transport/bin/sender" \
  --dest "${RX_IP}:${DATA_PORT}" --shm "$SHM_TX" --slots "$SLOTS" \
  --streams "$STREAMS" --src-port 20000 \
  --control-host "$RX_IP" --control-port "$CTRL_PORT" --idle-ms 3000 \
  >"$OUT/sender.log" 2>&1 &
sleep 0.3

sudo ip netns exec tx "$REPO/harness/bin/producer" \
  --shm "$SHM_TX" --slots "$SLOTS" --count "$COUNT" --rate "$RATE" \
  --type "$TYPE" >"$OUT/producer.log" 2>&1

wait || true

cat >"$OUT/params.json" <<EOF
{"count":$COUNT,"rate":$RATE,"streams":$STREAMS,"drop_pct":$DROP_PCT,
 "netem":"${NETEM:-off}","type":"$TYPE","slots":$SLOTS,"topology":"netns-veth",
 "note":"latency numbers from this topology are NOT meaningful; correctness only"}
EOF

echo
echo "===== sender =====";   sed -n '/---- sender ----/,$p'   "$OUT/sender.log"
echo "===== receiver ====="; sed -n '/---- receiver ----/,$p' "$OUT/receiver.log"
echo "===== consumer ====="; sed -n '/---- delivery/,$p'      "$OUT/consumer.log"
