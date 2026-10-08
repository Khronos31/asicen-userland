#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
export TMPDIR=/tmp
export MACOSX_DEPLOYMENT_TARGET=14.0
umask 022

usage() {
    printf '%s\n' 'usage: build-macos-static.sh --source-snapshot FILE.tar.gz --libusb-archive FILE.tar.bz2 --pcsc-include-dir DIR --pcsc-version VERSION --pcsc-license-expression EXPR --output DIR'
}
source_snapshot=
libusb_archive=
pcsc_include=
pcsc_version=
pcsc_license_expression=
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --source-snapshot) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_snapshot=$2; shift 2 ;;
        --libusb-archive) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; libusb_archive=$2; shift 2 ;;
        --pcsc-include-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; pcsc_include=$2; shift 2 ;;
        --pcsc-version) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; pcsc_version=$2; shift 2 ;;
        --pcsc-license-expression) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; pcsc_license_expression=$2; shift 2 ;;
        --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
        --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$source_snapshot" ] && [ -n "$libusb_archive" ] && [ -n "$pcsc_include" ] && \
    [ -n "$pcsc_version" ] && [ -n "$pcsc_license_expression" ] && [ -n "$output" ] || { usage >&2; exit 2; }
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || {
    printf '%s\n' 'native Apple silicon macOS is required' >&2; exit 1;
}
for tool in xcrun clang clang++ cmake ninja make python3 shasum tar nm otool lipo strip; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'required tool missing: %s\n' "$tool" >&2; exit 1; }
done

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
[ -f "$source_snapshot" ] && [ ! -L "$source_snapshot" ] || { printf '%s\n' 'invalid source snapshot' >&2; exit 1; }
[ -f "$libusb_archive" ] && [ ! -L "$libusb_archive" ] || { printf '%s\n' 'invalid libusb archive' >&2; exit 1; }
[ -d "$pcsc_include" ] && [ ! -L "$pcsc_include" ] && [ -f "$pcsc_include/ifdhandler.h" ] || {
    printf '%s\n' 'pcsc-lite include directory must contain ifdhandler.h and not be a symlink' >&2; exit 1;
}
source_snapshot=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve(strict=True))' "$source_snapshot")
pcsc_include=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve(strict=True))' "$pcsc_include")
libusb_archive=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve(strict=True))' "$libusb_archive")
case "$source_snapshot" in "$repo"/*) printf '%s\n' 'source snapshot must be outside the checkout' >&2; exit 1 ;; esac
case "$libusb_archive" in "$repo"/*) printf '%s\n' 'libusb archive must be outside the checkout' >&2; exit 1 ;; esac
libusb_sha=$(shasum -a 256 "$libusb_archive" | awk '{print $1}')
[ "$libusb_sha" = fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf ] || {
    printf '%s\n' 'libusb archive is not the pinned 1.0.30 source' >&2; exit 1;
}

parent=$(dirname -- "$output")
mkdir -p "$parent"
parent=$(CDPATH='' cd -- "$parent" && pwd -P)
output="$parent/$(basename -- "$output")"
case "$output/" in "$repo/"*) printf '%s\n' 'candidate must be outside the checkout' >&2; exit 1 ;; esac
[ ! -e "$output" ] && [ ! -L "$output" ] && [ ! -e "$output.tar.gz" ] && [ ! -L "$output.tar.gz" ] || {
    printf '%s\n' 'candidate output already exists' >&2; exit 1;
}

work=$(mktemp -d "$parent/.asicen-macos-work.XXXXXX")
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$work/src" "$work/libusb-src" "$work/libusb-prefix"
python3 "$repo/scripts/audit-linux-candidate.py" --source-archive "$source_snapshot"
tar -xzf "$source_snapshot" -C "$work/src"
cmp -s "$0" "$work/src/scripts/build-macos-static.sh" || {
    printf '%s\n' 'builder differs from the immutable source snapshot' >&2; exit 1;
}
tar -xjf "$libusb_archive" -C "$work/libusb-src" --strip-components=1
(
    cd "$work/libusb-src"
    ./configure --prefix="$work/libusb-prefix" --disable-shared --enable-static --with-pic \
        --disable-udev --disable-examples-build --disable-tests-build --disable-dependency-tracking
    make -j"$(sysctl -n hw.logicalcpu 2>/dev/null || printf '2')"
    make install
)
[ -f "$work/libusb-prefix/lib/libusb-1.0.a" ] || {
    printf '%s\n' 'static libusb archive was not produced' >&2; exit 1;
}
libs_private=$(sed -n 's/^Libs\.private:[[:space:]]*//p' "$work/libusb-prefix/lib/pkgconfig/libusb-1.0.pc")
for required in IOKit CoreFoundation Security; do
    case "$libs_private" in
        *"-framework $required"*|*"-framework,$required"*|*"-Wl,-framework,$required"*) ;;
        *)
        printf 'static libusb metadata is missing -framework %s: %s\n' "$required" "$libs_private" >&2
        exit 1 ;;
    esac
done
case "$libs_private" in *-lobjc*) ;; *) printf '%s\n' 'libusb private flags omit -lobjc' >&2; exit 1 ;; esac

extra_link_options='SHELL:-framework IOKit;SHELL:-framework CoreFoundation;SHELL:-framework Security;-lobjc'
build="$work/build"
cmake -S "$work/src" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_SKIP_RPATH=ON -DASICEN_ENABLE_LIBUSB=ON -DASICEN_ENABLE_IFD=ON \
    -DASICEN_REQUIRE_IFD=ON -DASICEN_BUILD_TESTS=ON \
    -DASICEN_PCSC_IFD_INCLUDE_DIR="$pcsc_include" \
    -DASICEN_LIBUSB_INCLUDE_DIR="$work/libusb-prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$work/libusb-prefix/lib/libusb-1.0.a" \
    "-DASICEN_LIBUSB_EXTRA_LINK_OPTIONS=$extra_link_options"
cmake --build "$build" --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || printf '2')"
(cd "$build" && ctest --output-on-failure --no-tests=error)

nm_output=$(nm "$build/asicend")
for symbol in libusb_init libusb_open libusb_close; do
    printf '%s\n' "$nm_output" | grep -E "[[:space:]][Tt][[:space:]]+_${symbol}$" >/dev/null || {
        printf 'static libusb symbol missing before stripping: %s\n' "$symbol" >&2; exit 1;
    }
done
[ -d "$build/ASICEN-IFD.bundle" ] || { printf '%s\n' 'ASICEN IFD bundle was not produced' >&2; exit 1; }
for binary in asicend asicen-ts asicenctl; do
    strip -S -x "$build/$binary"
done
strip -S -x "$build/libifd-asicen.dylib"
strip -S -x "$build/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib"
python3 "$work/src/scripts/package-macos-candidate.py" \
    --build-dir "$build" --libusb-source-dir "$work/libusb-src" \
    --source-snapshot "$source_snapshot" --libusb-archive "$libusb_archive" \
    --pcsc-include-dir "$pcsc_include" --pcsc-version "$pcsc_version" \
    --pcsc-license-expression "$pcsc_license_expression" \
    --output "$output"
python3 "$work/src/scripts/audit-macos-candidate.py" \
    --archive "$output.tar.gz"
printf 'firmware-free macOS intermediate candidate: %s.tar.gz\n' "$output"
