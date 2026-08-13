#!/usr/bin/env bash
# Arm or clear netem loss on the tx->rx direction.
#
#   bench/netns-loss.sh 1%                      independent (Bernoulli)
#   bench/netns-loss.sh gemodel 1% 10% 70% 0.1% bursty (Gilbert-Elliott)
#   bench/netns-loss.sh off
#
# Bursty loss is not a nicety: real loss arrives in runs, and runs are what
# defeat a single parity packet and a zero-stagger duplicate. The adaptive
# stagger is derived from the measured run-length histogram, so we need a
# generator that actually produces runs (plans/00_TRADEOFFS.txt D-25).
#
# This is the *secondary* injector. The primary one lives inside the receiver
# and is keyed on hash(dgram_id): deterministic, kernel-independent, and
# reproducible by a judge without root (D-14). netem is here to cross-check
# that our own dropper behaves like the real kernel path.
set -euo pipefail

TX_NS="${TX_NS:-tx}"
TX_IF="${TX_IF:-veth-tx}"

if [ "$(id -u)" -ne 0 ]; then exec sudo -E "$0" "$@"; fi
if [ $# -eq 0 ]; then echo "usage: $0 <pct%|gemodel p r 1-h 1-k|off>" >&2; exit 2; fi

ip netns exec "$TX_NS" tc qdisc del dev "$TX_IF" root 2>/dev/null || true

if [ "$1" = "off" ]; then
  echo "netem cleared on ${TX_NS}/${TX_IF}"
  exit 0
fi

ip netns exec "$TX_NS" tc qdisc add dev "$TX_IF" root netem loss "$@"
ip netns exec "$TX_NS" tc -s qdisc show dev "$TX_IF"
