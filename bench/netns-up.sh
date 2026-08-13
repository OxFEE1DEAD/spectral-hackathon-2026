#!/usr/bin/env bash
# Two network namespaces joined by a veth pair -- the local stand-in for two
# hosts. Real sockets, real kernel network stack, real packet loss when netem
# is armed, so sender/receiver can be developed and functionally verified end
# to end without renting anything.
#
# What this CANNOT tell us: any latency number. There is no core isolation, no
# NIC, no fabric. Numbers come from AWS (plans/01_AWS_SETUP.txt); this gives us
# correctness.
#
# Note that namespaces isolate the network only. /dev/shm is shared, so the
# producer-side and consumer-side rings must carry different names.
set -euo pipefail

TX_NS="${TX_NS:-tx}"
RX_NS="${RX_NS:-rx}"
TX_IF="${TX_IF:-veth-tx}"
RX_IF="${RX_IF:-veth-rx}"
TX_IP="${TX_IP:-10.0.0.1}"
RX_IP="${RX_IP:-10.0.0.2}"
PREFIX="${PREFIX:-24}"
MTU="${MTU:-1500}"

if [ "$(id -u)" -ne 0 ]; then exec sudo -E "$0" "$@"; fi

# Idempotent: tear down whatever is left from a previous run first.
ip netns del "$TX_NS" 2>/dev/null || true
ip netns del "$RX_NS" 2>/dev/null || true

ip netns add "$TX_NS"
ip netns add "$RX_NS"

ip link add "$TX_IF" mtu "$MTU" type veth peer name "$RX_IF" mtu "$MTU"
ip link set "$TX_IF" netns "$TX_NS"
ip link set "$RX_IF" netns "$RX_NS"

ip -n "$TX_NS" addr add "${TX_IP}/${PREFIX}" dev "$TX_IF"
ip -n "$RX_NS" addr add "${RX_IP}/${PREFIX}" dev "$RX_IF"
ip -n "$TX_NS" link set "$TX_IF" up
ip -n "$RX_NS" link set "$RX_IF" up
ip -n "$TX_NS" link set lo up
ip -n "$RX_NS" link set lo up

# Segmentation offloads on veth would coalesce datagrams and hide framing bugs
# that a real NIC path would expose. We want the boring, honest path here.
ip netns exec "$TX_NS" ethtool -K "$TX_IF" gro off gso off tso off 2>/dev/null || true
ip netns exec "$RX_NS" ethtool -K "$RX_IF" gro off gso off tso off 2>/dev/null || true

# Give the sockets room; the defaults are small enough to drop under a burst
# and would look like network loss. net.core.[rw]mem_max are global rather than
# per-namespace, so they are set once on the host and inherited.
sysctl -q -w net.core.wmem_max=16777216 net.core.rmem_max=16777216

echo "netns up: ${TX_NS}(${TX_IP}) <--${TX_IF}/${RX_IF}, mtu ${MTU}--> ${RX_NS}(${RX_IP})"
echo "  run in tx:  sudo ip netns exec ${TX_NS} <cmd>"
echo "  run in rx:  sudo ip netns exec ${RX_NS} <cmd>"
echo "  add loss:   bench/netns-loss.sh 1%"
