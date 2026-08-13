#!/usr/bin/env bash
# Bring up the local Linux development VM.
#
# Why a VM and not a container: we need free rein over network namespaces,
# tc/netem, sysctls and (later) kernel modules. Docker on macOS runs inside a
# Linux VM anyway, so a container costs us privileges without saving a layer.
#
# Why Ubuntu 24.04: it is one of the two distributions the judges run, so the
# commands in docs/REPRODUCE.md are written against it and carry over to AWS.
#
# Why arm64 rather than emulated x86_64: latency numbers are meaningless here
# either way, and native virtualisation iterates 10-20x faster. The x86-only
# code (rdtscp) sits behind #ifdef and is compile-checked separately.
set -euo pipefail

VM="${VM:-spectral}"
CPUS="${CPUS:-6}"
MEM="${MEM:-8}"
DISK="${DISK:-40}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if ! command -v limactl >/dev/null 2>&1; then
  echo "limactl not found. brew install lima" >&2
  exit 1
fi

if limactl list --quiet 2>/dev/null | grep -qx "$VM"; then
  echo "== instance '$VM' exists"
else
  echo "== creating instance '$VM' (ubuntu-24.04, ${CPUS} vCPU, ${MEM}GiB, ${DISK}GiB)"
  limactl create --tty=false --name="$VM" \
    --cpus="$CPUS" --memory="$MEM" --disk="$DISK" \
    --mount="${REPO}:w" \
    --mount-writable \
    template://ubuntu-24.04
fi

if [ "$(limactl list --format '{{.Status}}' "$VM" 2>/dev/null || true)" != "Running" ]; then
  echo "== starting '$VM'"
  limactl start --tty=false "$VM"
fi

echo "== provisioning packages"
limactl shell "$VM" -- sudo env DEBIAN_FRONTEND=noninteractive bash -s <<'PROVISION'
set -eux -o pipefail
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
  build-essential g++ make git pkg-config \
  iproute2 ethtool tcpdump iputils-ping netcat-openbsd socat \
  numactl util-linux linux-tools-common \
  python3-venv python3-pip
PROVISION

echo
echo "== ready"
echo "   repo mounted at: ${REPO}"
echo "   shell:  limactl shell ${VM}"
echo "   run:    limactl shell ${VM} --workdir ${REPO} -- <cmd>"
