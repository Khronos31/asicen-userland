#!/usr/bin/env bash
set -euo pipefail

daemon="$1"
client="$2"
instance="mock-integration"
temporary_root="${TMPDIR:-/tmp}"
[[ "$temporary_root" == /* ]] || temporary_root=/tmp
while [[ "$temporary_root" != / && "$temporary_root" == */ ]]; do
  temporary_root="${temporary_root%/}"
done
work="$(mktemp -d "${temporary_root%/}/asicen-mock.XXXXXX")"
# Match the pinned fixture policy, including the complete endpoint suffix.
# macOS has the smallest supported sun_path (104 bytes, including its NUL).
endpoint="$work/asicen-userland/$instance/control.sock"
if [[ "$(printf '%s' "$endpoint" | LC_ALL=C wc -c)" -ge 103 ]]; then
  rmdir "$work"
  work="$(mktemp -d /tmp/asicen-mock.XXXXXX)"
fi
sock="$work/asicen-userland/$instance/control.sock"
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

"$daemon" --runtime-dir "$work" --instance "$instance" > /dev/null 2>"$log" &
pid=$!

for _ in $(seq 1 100); do
  [[ -S "$sock" ]] && break
  sleep 0.02
done
[[ -S "$sock" ]]

"$client" --runtime-dir "$work" --instance "$instance" --receiver 1 \
  --channel T27 --packet-count 32 --output - > "$out"

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
