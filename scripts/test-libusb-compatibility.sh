#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise the platform-specific libusb configure and compile paths.
set -eu

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/asicen-libusb-compat.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

pkg_dir=$work/pkgconfig
prefix=$work/prefix
mkdir -p "$pkg_dir" "$prefix/include/libusb-1.0" "$prefix/lib"
printf '%s\n' '#define LIBUSB_API_VERSION 0x01000102' > "$prefix/include/libusb-1.0/libusb.h"
touch "$prefix/lib/libusb-1.0.so"
# Keep these pkg-config variable references literal; pkg-config expands them
# when it parses the generated fixture.
# shellcheck disable=SC2016
printf '%s\n' \
    "prefix=$prefix" \
    'exec_prefix=${prefix}' \
    'libdir=${prefix}/lib' \
    'includedir=${prefix}/include' \
    '' \
    '[...]' \
    'Name: libusb-1.0' \
    'Description: FreeBSD base libusb compatibility fixture' \
    'Version: 1.0.16' \
    'Libs: -L${libdir} -lusb-1.0' \
    'Cflags: -I${includedir}/libusb-1.0' > "$pkg_dir/libusb-1.0.pc"

set -- -G Ninja \
    -DCMAKE_CXX_COMPILER_WORKS=TRUE \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DASICEN_BUILD_TESTS=OFF \
    -DASICEN_BUILD_TOOLS=OFF \
    -DASICEN_BUILD_PCSC_IFD=OFF

freebsd_build=$work/freebsd-build
PKG_CONFIG_PATH="$pkg_dir" PKG_CONFIG_LIBDIR="$pkg_dir" cmake -S "$root" -B "$freebsd_build" \
    -DCMAKE_SYSTEM_NAME=FreeBSD "$@" >/dev/null
grep -F -- "$prefix/include/libusb-1.0" "$freebsd_build/build.ninja" >/dev/null
grep -F -- '-lusb-1.0' "$freebsd_build/build.ninja" >/dev/null

linux_build=$work/linux-old-build
if PKG_CONFIG_PATH="$pkg_dir" PKG_CONFIG_LIBDIR="$pkg_dir" cmake -S "$root" -B "$linux_build" \
    -DCMAKE_SYSTEM_NAME=Linux "$@" -DCMAKE_FIND_ROOT_PATH="$work/empty-root" \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY >/dev/null 2>&1; then
    printf '%s\n' 'Linux accepted the FreeBSD-only libusb 1.0.16 fixture' >&2
    exit 1
fi

override_build=$work/override-build
PKG_CONFIG_PATH="$pkg_dir" PKG_CONFIG_LIBDIR="$pkg_dir" cmake -S "$root" -B "$override_build" \
    "$@" \
    -DASICEN_LIBUSB_INCLUDE_DIR="$prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$prefix/lib/libusb-1.0.so" >/dev/null

if [ "$(uname -s)" = FreeBSD ] &&
    command -v c++ >/dev/null 2>&1 && pkg-config --cflags libusb-1.0 >/dev/null 2>&1; then
    libusb_include=$(pkg-config --variable=includedir libusb-1.0)
    c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti \
        -isystem "$libusb_include/libusb-1.0" \
        -I"$root/userland/include" -I"$root/userland/src" \
        -c "$root/userland/src/libusb_transport.cpp" -o "$work/libusb_transport-freebsd.o"
fi

printf '%s\n' 'libusb FreeBSD compatibility tests: PASS'
