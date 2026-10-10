#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Prove that a modified LGPL libusb is linked into the Windows asicend.exe.
# One modified-source build is compared with an already-built baseline.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
archive=
baseline=
cache=
while [ "$#" -gt 0 ]; do
    case "$1" in
    --libusb-source-archive)
        [ "$#" -ge 2 ] || exit 2
        archive=$2
        shift 2
        ;;
    --baseline-asicend)
        [ "$#" -ge 2 ] || exit 2
        baseline=$2
        shift 2
        ;;
    --cache)
        [ "$#" -ge 2 ] || exit 2
        cache=$2
        shift 2
        ;;
    --help)
        printf '%s\n' "usage: $0 --libusb-source-archive FILE --baseline-asicend FILE [--cache DIR]"
        exit 0
        ;;
    *)
        printf '%s\n' "unknown argument: $1" >&2
        exit 2
        ;;
    esac
done
[ -f "$archive" ] || { printf '%s\n' '--libusb-source-archive is required' >&2; exit 2; }
[ -f "$baseline" ] || { printf '%s\n' '--baseline-asicend is required' >&2; exit 2; }
command -v python3 >/dev/null || { printf '%s\n' 'python3 is required' >&2; exit 1; }
command -v strings >/dev/null || { printf '%s\n' 'strings is required' >&2; exit 1; }
work=$(mktemp -d /tmp/asicen-windows-static-relink.XXXXXX)
cleanup() { find "$work" -depth -delete; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$work/libusb" "$work/modified-source" "$work/modified"
tar -xjf "$archive" -C "$work/libusb" --strip-components=1
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
set -- --output "$work/modified" --libusb-source-dir "$work/modified-source"
if [ -n "$cache" ]; then
    set -- "$@" --cache "$cache"
fi
"$root/scripts/build-windows.sh" "$@"
if [ -e "$work/modified/libusb-1.0.dll" ]; then
    printf '%s\n' 'relink staged libusb-1.0.dll' >&2
    exit 1
fi
if cmp -s "$baseline" "$work/modified/asicend.exe"; then
    printf '%s\n' 'relink did not change asicend.exe' >&2
    exit 1
fi
modified_marker_count=$(strings "$work/modified/asicend.exe" | grep -F -c 'libusb.relink-test.invalid' || true)
[ "$modified_marker_count" -ge 1 ] || {
    printf '%s\n' 'modified libusb marker is absent from relinked asicend.exe' >&2
    exit 1
}
if strings "$baseline" | grep -F 'libusb.relink-test.invalid' >/dev/null; then
    printf '%s\n' 'modified marker unexpectedly present in baseline asicend.exe' >&2
    exit 1
fi
if ! strings "$baseline" | grep -F 'https://libusb.info' >/dev/null; then
    printf '%s\n' 'baseline asicend.exe does not contain the static libusb version URL' >&2
    exit 1
fi
printf '%s\n' 'Windows static libusb relink incorporated modified source: PASS'
