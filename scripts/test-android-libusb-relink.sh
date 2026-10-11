#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Clean-tree proof that a modified LGPL libusb is incorporated in a relink.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
archive=
abi=x86_64
while [ "$#" -gt 0 ]; do
    case "$1" in
    --abi) [ "$#" -ge 2 ] || exit 2; abi=$2; shift 2 ;;
    --libusb-archive|--libusb-source-archive)
        [ "$#" -ge 2 ] || exit 2
        archive=$2
        shift 2
        ;;
    --help)
        printf '%s\n' "usage: $0 [--abi x86_64|aarch64|armv7a] --libusb-archive FILE"
        exit 0
        ;;
    *)
        printf '%s\n' "unknown argument: $1" >&2
        exit 2
        ;;
    esac
done
[ -f "$archive" ] || { printf '%s\n' '--libusb-source-archive is required' >&2; exit 2; }
command -v python3 >/dev/null || { printf '%s\n' 'python3 is required' >&2; exit 1; }
work=$(mktemp -d /tmp/asicen-android-relink.XXXXXX)
cleanup() { find "$work" -depth -delete; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$work/libusb" "$work/original-source" "$work/modified-source" "$work/original" "$work/modified"
tar -xjf "$archive" -C "$work/libusb" --strip-components=1
cp -a "$work/libusb/." "$work/original-source/"
cp -a "$work/libusb/." "$work/modified-source/"
python3 - "$work/modified-source/libusb/core.c" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
data = p.read_text()
old = "https://libusb.info"
new = "https://libusb.relink-test.invalid"
if data.count(old) != 2:
    raise SystemExit(f"expected two libusb version URLs before replacement, found {data.count(old)}")
modified = data.replace(old, new)
if modified.count(new) != 2:
    raise SystemExit(f"expected two replacement URLs after replacement, found {modified.count(new)}")
p.write_text(modified, newline="\n")
PY
case "$abi" in
    aarch64|arm64-v8a) suffix=arm64-v8a ;;
    armv7a|armeabi-v7a) suffix=armeabi-v7a ;;
    x86_64) suffix=x86_64 ;;
    *) printf '%s\n' "unsupported ABI: $abi" >&2; exit 2 ;;
esac
"$root/scripts/build-android.sh" --abi "$abi" --output "$work/original" --libusb-source "$work/original-source"
"$root/scripts/build-android.sh" --abi "$abi" --output "$work/modified" --libusb-source "$work/modified-source"
if cmp -s "$work/original/asicend-$suffix" "$work/modified/asicend-$suffix"; then
    printf '%s\n' 'relink did not change asicend' >&2
    exit 1
fi
modified_marker_count=$(strings "$work/modified/asicend-$suffix" | grep -F -c 'libusb.relink-test.invalid' || true)
[ "$modified_marker_count" -ge 1 ] || {
    printf '%s\n' 'modified libusb marker is absent from relinked asicend' >&2
    exit 1
}
if strings "$work/original/asicend-$suffix" | grep -F 'libusb.relink-test.invalid' >/dev/null; then
    printf '%s\n' 'modified marker unexpectedly present in original asicend' >&2
    exit 1
fi
printf '%s\n' 'Android static libusb relink incorporated modified source: PASS'
