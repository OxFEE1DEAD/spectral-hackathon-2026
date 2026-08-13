#!/usr/bin/env bash
# Correctness gate. Runs on every commit, needs no AWS and no tuning.
#
# It deliberately asserts nothing about latency: this topology cannot produce a
# meaningful number, and a threshold on one would either be noise-triggered or
# so loose as to be useless. What it does assert is everything that must hold
# regardless of hardware -- no loss without injected loss, no duplicate
# published, redundancy recovering what it is supposed to recover.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"
fails=0

step() { printf '\n== %s\n' "$*"; }
pass() { printf '   ok   %s\n' "$*"; }
fail() { printf '   FAIL %s\n' "$*"; fails=$((fails + 1)); }

step "build"
make -C harness   >/dev/null 2>&1 || { fail "harness build"; exit 1; }
make -C transport >/dev/null 2>&1 || { fail "transport build"; exit 1; }
pass "harness + transport"

step "unit tests"
make -C harness test   >/dev/null 2>&1 && pass "harness" || fail "harness"
make -C transport test >/dev/null 2>&1 && pass "transport" || fail "transport"

if ! ip netns list 2>/dev/null | grep -q '^tx'; then
  sudo "$REPO/bench/netns-up.sh" >/dev/null 2>&1 || { fail "netns setup"; exit 1; }
fi

# field <log> <label>  -- pull "label : value" out of a report
field() { sed -n "s/^ *$2 *: *\([0-9.]*\).*/\1/p" "$1" | head -1; }

run_case() {
  local name="$1"; shift
  local out
  out="$(mktemp -d)"
  "$REPO/bench/e2e-netns.sh" --out "$out" "$@" >/dev/null 2>&1
  echo "$out"
}

step "clean path: no loss, nothing dropped, nothing duplicated"
OUT="$(run_case clean --count 120000 --rate 150000)"
recv=$(field "$OUT/consumer.log" received)
exp=$(field "$OUT/consumer.log" expected)
drop=$(field "$OUT/consumer.log" dropped)
pub=$(sed -n 's/^messages *: *[0-9]* parsed, \([0-9]*\) published.*/\1/p' "$OUT/receiver.log")
lap=$(sed -n 's/^ring lapped *: *\([0-9]*\).*/\1/p' "$OUT/sender.log")

[ "${drop:-x}" = "0" ] && pass "dropped == 0" || fail "dropped == 0 (got '${drop:-none}')"
[ "${recv:-x}" = "${exp:-y}" ] && pass "received == expected ($recv)" \
                              || fail "received($recv) != expected($exp)"
# If these diverge, the consumer is being lapped by us and its drop count is
# measuring its own slowness, not the transport's.
[ "${pub:-x}" = "${recv:-y}" ] && pass "our published == their received ($pub)" \
                              || fail "published($pub) != received($recv)"
[ "${lap:-x}" = "0" ] && pass "producer ring never lapped" \
                      || fail "sender lapped the producer ring ($lap events)"
[ "${recv:-0}" -gt 50000 ] && pass "delivered a meaningful sample ($recv)" \
                           || fail "too few messages delivered ($recv)"
rm -rf "$OUT"

step "single stream under 1% injected loss: loss is visible"
OUT="$(run_case loss1 --count 120000 --rate 150000 --streams 1 --drop-pct 1.0)"
rate=$(field "$OUT/consumer.log" drop_rate)
awk -v r="${rate:-0}" 'BEGIN{exit !(r>0.5 && r<1.6)}' \
  && pass "drop_rate ${rate}% is around the injected 1%" \
  || fail "drop_rate ${rate}% not near injected 1%"
rm -rf "$OUT"

step "two streams under the same loss: recovered to p-squared"
OUT="$(run_case dup2 --count 120000 --rate 150000 --streams 2 --drop-pct 1.0)"
rate=$(field "$OUT/consumer.log" drop_rate)
dups=$(sed -n 's/.*duplicates=\([0-9]*\).*/\1/p' "$OUT/receiver.log" | head -1)
awk -v r="${rate:-9}" 'BEGIN{exit !(r<0.05)}' \
  && pass "drop_rate ${rate}% collapsed from ~1% (predicted p^2 = 0.01%)" \
  || fail "drop_rate ${rate}% did not collapse"
[ "${dups:-0}" -gt 10000 ] && pass "dedup absorbed $dups duplicate copies" \
                           || fail "dedup saw only ${dups:-0} duplicates"
rm -rf "$OUT"

step "three streams: clean"
OUT="$(run_case dup3 --count 120000 --rate 150000 --streams 3 --drop-pct 1.0)"
rate=$(field "$OUT/consumer.log" drop_rate)
awk -v r="${rate:-9}" 'BEGIN{exit !(r<0.005)}' \
  && pass "drop_rate ${rate}% (predicted p^3 = 0.0001%)" \
  || fail "drop_rate ${rate}% too high for three copies"
rm -rf "$OUT"

printf '\n'
if [ "$fails" -eq 0 ]; then
  echo "REGRESS OK"
  exit 0
fi
echo "REGRESS FAILED: $fails check(s)"
exit 1
