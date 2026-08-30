#!/usr/bin/env bash
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
key="${SSH_KEY:-$HOME/.ssh/id_ed25519}"
ssh_opts=(-i "$key" -o StrictHostKeyChecking=accept-new -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)

tf() { terraform -chdir="$here" "$@"; }
out() { tf output -raw "$1"; }
remote() { ssh "${ssh_opts[@]}" "ubuntu@$1" "${@:2}"; }

wait_ready() {
  local host="$1" n=0
  until remote "$host" 'test -f /var/lib/cloud/bench-ready' 2>/dev/null; do
    n=$((n + 1))
    [[ $n -gt 60 ]] && { echo "host $host never became ready" >&2; exit 1; }
    sleep 10
  done
}

cmd_up() {
  tf init -input=false
  tf apply -input=false -auto-approve \
    -var "ssh_cidr=$(curl -fsS https://checkip.amazonaws.com | tr -d '\n')/32" \
    -var "ssh_public_key=$(cat "$key.pub")"
  cmd_sync
}

cmd_sync() {
  local s r
  s="$(out sender_public_ip)"
  r="$(out receiver_public_ip)"
  for h in "$s" "$r"; do
    wait_ready "$h"
    rsync -az -e "ssh ${ssh_opts[*]}" \
      --exclude='.git/' --exclude='.venv/' --exclude='*/bin/' --exclude='results/' \
      "$root/" "ubuntu@$h:spectral/"
    remote "$h" 'cd spectral && make -C harness && make -C transport && make -C agent-solution/harness && make -C agent-solution/transport'
  done
  cmd_info
}

cmd_info() {
  cat <<INFO

sender    ssh -i $key ubuntu@$(out sender_public_ip)    private $(out sender_private_ip)    isolated $(out sender_isolated_cores)
receiver  ssh -i $key ubuntu@$(out receiver_public_ip)  private $(out receiver_private_ip)  isolated $(out receiver_isolated_cores)
zone      $(out availability_zone)

receiver first:
  cd spectral && scripts/bench.sh --role recv --bind $(out receiver_private_ip) \
    --receiver-cores "1" --consumer-cores "2" --reps 10 --samples 500000 --out-dir results/aws

then sender:
  cd spectral && scripts/bench.sh --role send --peer $(out receiver_private_ip) \
    --producer-core 1 --sender-core 2 --rate 100000

INFO
}

cmd_check() {
  remote "$(out receiver_public_ip)" 'cd spectral && scripts/check_cores.sh --secs 2 1 2'
}

cmd_fetch() {
  rsync -az -e "ssh ${ssh_opts[*]}" \
    "ubuntu@$(out receiver_public_ip):spectral/results/" "$root/results/aws/"
  echo "pulled into $root/results/aws/"
}

cmd_down() {
  tf destroy -input=false -auto-approve \
    -var "ssh_cidr=0.0.0.0/32" -var "ssh_public_key=$(cat "$key.pub")"
}

case "${1:-}" in
  up|sync|info|check|fetch|down) "cmd_$1" ;;
  *) echo "usage: $0 {up|sync|info|check|fetch|down}" >&2; exit 2 ;;
esac
