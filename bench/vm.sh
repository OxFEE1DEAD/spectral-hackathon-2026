#!/usr/bin/env bash
# Run a command inside the development VM, from the repo root.
#   bench/vm.sh make -C transport test
#   bench/vm.sh          # interactive shell
set -euo pipefail
VM="${VM:-spectral}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ $# -eq 0 ]; then
  exec limactl shell --workdir "$REPO" "$VM"
fi
exec limactl shell --workdir "$REPO" "$VM" "$@"
