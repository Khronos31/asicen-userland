#!/usr/bin/env bash
set -euo pipefail

daemon="$1"
client="$2"
work="$(mktemp -d)"
sock="$work/asicen.sock"
out="$work/out.ts"
log="$work/daemon.log"

cleanup() {
  if [[ -n "${pid:-}" ]]; then
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT

"$daemon" --mock --socket "$sock" > /dev/null 2>"$log" &
pid=$!

for _ in $(seq 1 100); do
  [[ -S "$sock" ]] && break
  sleep 0.02
done
[[ -S "$sock" ]]

"$client" --socket "$sock" --receiver 1 --packet-count 32 > "$out"

python3 - "$out" <<'PY'
import sys
p=open(sys.argv[1],'rb').read()
assert len(p)==32*188, len(p)
for i in range(32):
    q=p[i*188:(i+1)*188]
    assert q[0]==0x47
    assert q[1]==0x1f and q[2]==0xff
    assert (q[3]&0x0f)==(i&0x0f)
print("mock stream ok")
PY
