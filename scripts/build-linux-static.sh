#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the libc-independent production executables in a musl environment.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=
source_archive=
source_dir=
usage() { printf '%s\n' "usage: $0 --output DIR [--libusb-source-archive FILE]"; }
while [ "$#" -gt 0 ]; do
    case "$1" in
    --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
    --libusb-source-archive) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_archive=$2; shift 2 ;;
    --libusb-source-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_dir=$2; shift 2 ;;
    --help) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$output" ] || { usage >&2; exit 2; }
case "$(uname -m)" in x86_64|aarch64) ;; *) printf '%s\n' 'unsupported architecture' >&2; exit 1 ;; esac
command -v cmake >/dev/null || { printf '%s\n' 'cmake is required' >&2; exit 1; }
make_program=
if command -v ninja >/dev/null; then make_program=$(command -v ninja)
elif command -v samu >/dev/null; then make_program=$(command -v samu)
elif command -v samurai >/dev/null; then make_program=$(command -v samurai)
else printf '%s\n' 'ninja or samurai is required' >&2; exit 1
fi
command -v gcc >/dev/null || { printf '%s\n' 'a musl gcc is required' >&2; exit 1; }
command -v python3 >/dev/null || { printf '%s\n' 'python3 is required' >&2; exit 1; }
compiler_target=$(gcc -dumpmachine)
case "$compiler_target" in
    *linux-musl*) ;;
    *) printf '%s\n' "a musl target compiler is required: $compiler_target" >&2; exit 1 ;;
esac
libc_archive=$(realpath "$(gcc -print-file-name=libc.a)")
libc_loader=$(realpath "$(gcc -print-file-name=libc.so)")
[ -f "$libc_archive" ] && [ -x "$libc_loader" ] || {
    printf '%s\n' 'compiler-selected musl archive/loader is missing' >&2
    exit 1
}
libc_identification=$("$libc_loader" 2>&1 || true)
printf '%s\n' "$libc_identification" | grep -E '^musl libc \(' >/dev/null || {
    printf '%s\n' 'compiler-selected loader is not musl' >&2
    exit 1
}
printf '%s\n' "$libc_identification" | grep -F 'Version 1.2.5' >/dev/null || {
    printf '%s\n' 'the validated release libc is musl 1.2.5' >&2
    exit 1
}
# Keep only stable identification lines, never the executable's build path.
libc_identification=$(printf '%s\n' "$libc_identification" | sed -n '/^musl libc /p; /^Version /p')
libc_archive_sha256=$(sha256sum "$libc_archive" | awk '{print $1}')
nm_tool=$(command -v nm || command -v llvm-nm || true)
strip_tool=$(command -v strip || command -v llvm-strip || true)
[ -n "$nm_tool" ] || { printf '%s\n' 'target-native nm is required' >&2; exit 1; }
[ -n "$strip_tool" ] || { printf '%s\n' 'target-native strip is required' >&2; exit 1; }
mkdir -p "$output"
output=$(CDPATH='' cd -- "$output" && pwd)
work=$(mktemp -d /tmp/asicen-linux-static.XXXXXX)
cleanup() { find "$work" -depth -delete; }
trap cleanup EXIT HUP INT TERM
archive=$work/libusb-1.0.30.tar.bz2
if [ -n "$source_archive" ] && [ -n "$source_dir" ]; then
    printf '%s\n' 'choose only one libusb source input' >&2
    exit 2
elif [ -n "$source_dir" ]; then
    [ -d "$source_dir" ] && [ -x "$source_dir/configure" ] && [ -f "$source_dir/COPYING" ] || {
        printf '%s\n' "invalid libusb source directory: $source_dir" >&2
        exit 1
    }
    mkdir -p "$work/libusb"
    cp -a "$source_dir/." "$work/libusb/"
elif [ -n "$source_archive" ]; then
    [ -f "$source_archive" ] || { printf '%s\n' "libusb archive not found: $source_archive" >&2; exit 1; }
    cp "$source_archive" "$archive"
else
    command -v curl >/dev/null || { printf '%s\n' 'curl or --libusb-source-archive is required' >&2; exit 1; }
    curl -fsSL --retry 2 -o "$archive" \
        https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
fi
expected=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
if [ -z "$source_dir" ]; then
    actual=$(sha256sum "$archive" | awk '{print $1}')
    [ "$actual" = "$expected" ] || { printf '%s\n' "libusb checksum mismatch: $actual" >&2; exit 1; }
fi
mkdir -p "$work/libusb" "$work/prefix"
[ -n "$source_dir" ] || tar -xjf "$archive" -C "$work/libusb" --strip-components=1
(cd "$work/libusb" && ./configure --prefix="$work/prefix" --disable-shared --enable-static \
    --with-pic --disable-udev --disable-examples-build --disable-tests-build \
    --disable-dependency-tracking && make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" && make install)
[ -f "$work/prefix/lib/libusb-1.0.a" ] || { printf '%s\n' 'static libusb was not built' >&2; exit 1; }
build=$work/build
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_MAKE_PROGRAM="$make_program" -DCMAKE_BUILD_TYPE=Release \
    -DASICEN_BUILD_TESTS=OFF -DASICEN_BUILD_PCSC_IFD=OFF -DASICEN_ENABLE_LIBUSB=ON \
    -DASICEN_RELEASE_ASICEND_NO_BUILD_ID=ON \
    -DASICEN_LIBUSB_INCLUDE_DIR="$work/prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$work/prefix/lib/libusb-1.0.a" \
    -DCMAKE_CXX_FLAGS='-static -static-libstdc++ -static-libgcc' \
    -DCMAKE_EXE_LINKER_FLAGS='-static -static-libstdc++ -static-libgcc' \
    -DASICEN_STATIC_LINK_MAP_DIR="$work/link-maps"
cmake --build "$build" --target asicend asicen-ts asicenctl

# Prove that each final executable selected the compiler's musl libc archive.
for program in asicend asicen-ts asicenctl; do
    python3 - "$work/link-maps/$program.map" "$libc_archive" "$build" <<'PYMAP'
import os
from pathlib import Path
import re
import sys
link_map = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
expected = Path(sys.argv[2]).resolve(strict=True)
archives = re.findall(r"([^\s()]+/libc\.a)(?:\(|\s)", link_map)
def resolve_archive(path):
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = Path(sys.argv[3]) / candidate
    return candidate.resolve(strict=True)
if not archives or any(resolve_archive(path) != expected for path in archives):
    raise SystemExit("final link did not use the verified musl libc archive")
PYMAP
done

# Verify static libusb provenance before removing the symbol table.  The
# release artifacts must retain runtime code, but never their build symbols.
nm_output=$("$nm_tool" "$build/asicend")
for symbol in libusb_init libusb_wrap_sys_device libusb_close; do
    printf '%s\n' "$nm_output" | grep -E "[[:space:]]$symbol$" >/dev/null || {
        printf '%s\n' "missing static libusb symbol in asicend: $symbol" >&2
        exit 1
    }
done

# Strip only after the static-link provenance check and before anything is
# copied into the release staging directory or audited.
for program in asicend asicen-ts asicenctl; do
    "$strip_tool" --strip-all "$build/$program"
done
for program in asicend asicen-ts asicenctl; do
    cp "$build/$program" "$output/$program"
    chmod 0755 "$output/$program"
done

# Retain hash-bound, path-free toolchain evidence for the final archive audit.
python3 - "$output" "$compiler_target" "$libc_archive_sha256" "$libc_identification" "$root" <<'PYMETA'
import hashlib
import json
from pathlib import Path
import subprocess
import sys
output = Path(sys.argv[1])
metadata = {
    "schema": 1,
    "source_ref": "working-tree",
    "compiler_target": sys.argv[2],
    "libc_archive_sha256": sys.argv[3],
    "libc_identification": sys.argv[4],
    "production_binary_sha256": {
        program: hashlib.sha256((output / program).read_bytes()).hexdigest()
        for program in ("asicend", "asicen-ts", "asicenctl")
    },
}
# A source bundle records the exact archived commit when .git is unavailable.
source_manifest = Path(sys.argv[5]).parent / "source-manifest.json"
try:
    commit = subprocess.check_output(
        ["git", "-c", "safe.directory=" + sys.argv[5], "-C", sys.argv[5], "rev-parse", "HEAD"], text=True).strip()
    status = subprocess.check_output(
        ["git", "-c", "safe.directory=" + sys.argv[5], "-C", sys.argv[5], "status", "--porcelain", "--untracked-files=no"], text=True)
    metadata["source_ref"] = "working-tree" if status.strip() else commit
except (subprocess.CalledProcessError, FileNotFoundError):
    if source_manifest.is_file():
        metadata["source_ref"] = json.loads(source_manifest.read_text())["repository_commit"]
(output / "build-metadata.json").write_text(
    json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PYMETA
