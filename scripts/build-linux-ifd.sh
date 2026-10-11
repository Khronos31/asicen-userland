#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the PC/SC IFD Handler for the host libc, separately from static CLIs.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
libc=
output=
usage() { printf '%s\n' "usage: $0 --libc glibc|musl --output DIR"; }
while [ "$#" -gt 0 ]; do
    case "$1" in
    --libc) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; libc=$2; shift 2 ;;
    --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
    --help) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
case "$libc" in glibc|musl) ;; *) printf '%s\n' '--libc must be glibc or musl' >&2; exit 2 ;; esac
[ -n "$output" ] || { usage >&2; exit 2; }
command -v cmake >/dev/null || { printf '%s\n' 'cmake is required' >&2; exit 1; }
make_program=
if command -v ninja >/dev/null; then make_program=$(command -v ninja)
elif command -v samu >/dev/null; then make_program=$(command -v samu)
elif command -v samurai >/dev/null; then make_program=$(command -v samurai)
else printf '%s\n' 'ninja or samurai is required' >&2; exit 1
fi
nm_tool=$(command -v nm || command -v llvm-nm || true)
strip_tool=$(command -v strip || command -v llvm-strip || true)
[ -n "$nm_tool" ] || { printf '%s\n' 'target-native nm is required' >&2; exit 1; }
[ -n "$strip_tool" ] || { printf '%s\n' 'target-native strip is required' >&2; exit 1; }
mkdir -p "$output"
output=$(CDPATH='' cd -- "$output" && pwd)
work=$(mktemp -d /tmp/asicen-linux-ifd.XXXXXX)
cleanup() { find "$work" -depth -delete; }
trap cleanup EXIT HUP INT TERM
compiler_target=$(g++ -dumpmachine)
if [ "$libc" = musl ]; then
    case "$compiler_target" in
        *linux-musl*) ;;
        *) printf '%s\n' "musl IFD requires a musl target compiler: $compiler_target" >&2; exit 1 ;;
    esac
fi
if [ "$libc" = glibc ]; then
    floor=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)
    case "$floor" in glibc\ 2.31) : ;; *)
        printf '%s\n' "glibc baseline 2.31 required; detected ${floor:-unknown}" >&2
        exit 1
        esac
fi
cmake -S "$root" -B "$work/build" -G Ninja -DCMAKE_MAKE_PROGRAM="$make_program" -DCMAKE_BUILD_TYPE=Release \
    -DASICEN_BUILD_TESTS=OFF -DASICEN_ENABLE_LIBUSB=OFF \
    -DASICEN_BUILD_PCSC_IFD=ON -DASICEN_REQUIRE_PCSC_IFD=ON \
    -DCMAKE_CXX_FLAGS='-fPIC -static-libstdc++ -static-libgcc' \
    -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc'
cmake --build "$work/build" --target asicen_ifdhandler
library=$work/build/libasicen-userland-ifd.so
verify_ifd_exports()
{
    symbols=$("$nm_tool" -D --defined-only "$1")
    for symbol in \
        IFDHCreateChannel IFDHCreateChannelByName IFDHCloseChannel \
        IFDHGetCapabilities IFDHSetCapabilities IFDHSetProtocolParameters \
        IFDHPowerICC IFDHTransmitToICC IFDHControl IFDHICCPresence; do
        printf '%s\n' "$symbols" | grep -E "[[:space:]]_?$symbol$" >/dev/null || {
            printf '%s\n' "missing exported IFD symbol: $symbol" >&2
            exit 1
        }
    done
}

# Prove the required IFD ABI before stripping, then preserve it through the
# release operation.  --strip-unneeded removes DWARF/.symtab without dropping
# symbols retained in the dynamic export set.
verify_ifd_exports "$library"
"$strip_tool" --strip-unneeded "$library"
verify_ifd_exports "$library"
cp "$library" "$output/asicen-userland-ifd.so"
chmod 0755 "$output/asicen-userland-ifd.so"

python3 - "$output" "$root" "$compiler_target" <<'PYMETA'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
output = Path(sys.argv[1])
metadata = {
    "schema": 1,
    "source_ref": "working-tree",
    "compiler_target": sys.argv[3],
    "library_sha256": hashlib.sha256((output / "asicen-userland-ifd.so").read_bytes()).hexdigest(),
}
source_manifest = Path(sys.argv[2]).parent / "source-manifest.json"
try:
    commit = subprocess.check_output(
        ["git", "-c", "safe.directory=" + sys.argv[2], "-C", sys.argv[2], "rev-parse", "HEAD"], text=True).strip()
    status = subprocess.check_output(
        ["git", "-c", "safe.directory=" + sys.argv[2], "-C", sys.argv[2], "status", "--porcelain", "--untracked-files=no"], text=True)
    metadata["source_ref"] = "working-tree" if status.strip() else commit
except (subprocess.CalledProcessError, FileNotFoundError):
    if source_manifest.is_file():
        metadata["source_ref"] = json.loads(source_manifest.read_text())["repository_commit"]
(output / "build-metadata.json").write_text(
    json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PYMETA
