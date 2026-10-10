#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the macOS arm64 production executables with the pinned libusb 1.0.30
# linked statically, so the release does not depend on a host libusb.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
build=
source_archive=
source_dir=
pcsc_include=
tests=OFF
usage() {
    printf '%s\n' "usage: $0 --build-dir DIR [--pcsc-include-dir DIR] [--tests] [--libusb-source-archive FILE | --libusb-source-dir DIR]"
}
while [ "$#" -gt 0 ]; do
    case "$1" in
    --build-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; build=$2; shift 2 ;;
    --pcsc-include-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; pcsc_include=$2; shift 2 ;;
    --tests) tests=ON; shift ;;
    --libusb-source-archive) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_archive=$2; shift 2 ;;
    --libusb-source-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_dir=$2; shift 2 ;;
    --help) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$build" ] || { usage >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { printf '%s\n' 'macOS is required' >&2; exit 1; }
[ "$(uname -m)" = arm64 ] || { printf '%s\n' 'unsupported architecture' >&2; exit 1; }
for tool in cmake ninja cc make nm otool shasum; do
    command -v "$tool" >/dev/null || { printf '%s\n' "$tool is required" >&2; exit 1; }
done
mkdir -p "$build"
build=$(CDPATH='' cd -- "$build" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/asicen-macos-static.XXXXXX")
cleanup() { find "$work" -depth -delete; }
trap cleanup EXIT HUP INT TERM
archive=$work/libusb-1.0.30.tar.bz2
if [ -n "$source_archive" ] && [ -n "$source_dir" ]; then
    printf '%s\n' 'choose only one libusb source input' >&2
    exit 2
elif [ -n "$source_dir" ]; then
    if [ ! -d "$source_dir" ] || [ ! -x "$source_dir/configure" ] || [ ! -f "$source_dir/COPYING" ]; then
        printf '%s\n' "invalid libusb source directory: $source_dir" >&2
        exit 1
    fi
    mkdir -p "$work/libusb"
    # Keep the timestamps: a plain copy makes configure.ac and the m4 inputs
    # look newer than configure, and make would then try to rerun autoconf.
    cp -pR "$source_dir/." "$work/libusb/"
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
    actual=$(shasum -a 256 "$archive" | awk '{print $1}')
    [ "$actual" = "$expected" ] || { printf '%s\n' "libusb checksum mismatch: $actual" >&2; exit 1; }
fi

# The static libusb is installed inside the build tree so that the CMake
# cache keeps pointing at an existing archive after this script exits.
prefix=$build/libusb-static
if [ -e "$prefix" ]; then find "$prefix" -depth -delete; fi
mkdir -p "$work/libusb"
[ -n "$source_dir" ] || tar -xjf "$archive" -C "$work/libusb" --strip-components=1
# libusb is built outside CMake, so give its compiler the same reproducibility
# contract as the Android libusb build (not a macOS project-build contract).
# The mktemp work tree varies per run, so map it to "." instead of letting it
# enter recorded file and debug paths.
. "$root/scripts/libusb-reproducibility.sh"
libusb_cflags=$(libusb_reproducible_cflags "$work/libusb" "$work")
(cd "$work/libusb" && CFLAGS="$libusb_cflags" ./configure --prefix="$prefix" --disable-shared --enable-static \
    --disable-examples-build --disable-tests-build --disable-dependency-tracking &&
    make -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 2)" && make install)
[ -f "$prefix/lib/libusb-1.0.a" ] || { printf '%s\n' 'static libusb was not built' >&2; exit 1; }

# A static libusb carries its darwin backend's system dependencies in
# Libs.private (libobjc and the IOKit, CoreFoundation, and Security
# frameworks). Take them from the libusb build itself instead of restating them.
libs_private=$(sed -n 's/^Libs\.private:[[:space:]]*//p' "$prefix/lib/pkgconfig/libusb-1.0.pc")
for framework in IOKit CoreFoundation Security; do
    case "$libs_private" in
    *"-framework,$framework"*|*"-framework $framework"*) ;;
    *) printf '%s\n' "libusb Libs.private lacks $framework: $libs_private" >&2; exit 1 ;;
    esac
done

set -- -DASICEN_BUILD_PCSC_IFD=OFF
if [ -n "$pcsc_include" ]; then
    set -- -DASICEN_REQUIRE_PCSC_IFD=ON -DASICEN_PCSC_IFD_INCLUDE_DIR:PATH="$pcsc_include"
fi
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DASICEN_BUILD_TESTS="$tests" -DASICEN_ENABLE_LIBUSB=ON \
    -DASICEN_LIBUSB_INCLUDE_DIR="$prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$prefix/lib/libusb-1.0.a" \
    -DASICEN_LIBUSB_LINK_LIBRARIES="$libs_private" \
    "$@"
cmake --build "$build"

# Verify static libusb provenance before packaging strips local symbols.
nm_output=$(nm "$build/asicend")
for symbol in libusb_init libusb_open libusb_close; do
    printf '%s\n' "$nm_output" | grep -E "[[:space:]]T _$symbol$" >/dev/null || {
        printf '%s\n' "missing static libusb symbol in asicend: $symbol" >&2
        exit 1
    }
done
for program in asicend asicen-ts asicenctl; do
    linkage=$(otool -L "$build/$program")
    printf '%s\n' "$linkage"
    if printf '%s\n' "$linkage" | grep -F 'libusb-1.0' >/dev/null; then
        printf '%s\n' "dynamic libusb dependency in $program" >&2
        exit 1
    fi
    # Only asicend links libusb, so only asicend may carry its darwin frameworks.
    if [ "$program" != asicend ] && printf '%s\n' "$linkage" | grep -F 'IOKit.framework' >/dev/null; then
        printf '%s\n' "libusb system frameworks leaked into $program" >&2
        exit 1
    fi
done
