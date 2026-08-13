#!/usr/bin/env bash
# Remove the local two-namespace topology. Deleting the namespaces takes the
# veth pair with them.
set -euo pipefail
TX_NS="${TX_NS:-tx}"
RX_NS="${RX_NS:-rx}"
if [ "$(id -u)" -ne 0 ]; then exec sudo -E "$0" "$@"; fi
ip netns del "$TX_NS" 2>/dev/null || true
ip netns del "$RX_NS" 2>/dev/null || true
echo "netns down"
