#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Cross-build the Windows x64 (Phase 1) production CLIs with a pinned
# llvm-mingw/UCRT toolchain. The toolchain and dependency versions and their
# SHA-256 values are fixed here; no compiler or dependency binary is vendored
# into the tracked tree. The PE/import audit and the deterministic zip are not
# done here: they are the integrated interfaces in audit-artifact.py and
# package-artifact.py.
set -eu

# Fixed timestamp so the PE headers are reproducible.
SOURCE_DATE_EPOCH=1700000000
export SOURCE_DATE_EPOCH

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
output=
test_output=
libusb_source_dir=
cache=${ASICEN_WINDOWS_CACHE:-"$root/.windows-toolchain"}
skip_toolchain_download=0
usage() {
    printf '%s\n' "usage: $0 --output DIR [--test-output DIR] [--cache DIR] [--skip-toolchain-download] [--libusb-source-dir DIR]"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
    --test-output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; test_output=$2; shift 2 ;;
    --cache) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; cache=$2; shift 2 ;;
    --libusb-source-dir) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; libusb_source_dir=$2; shift 2 ;;
    --skip-toolchain-download) skip_toolchain_download=1; shift ;;
    --help) usage; exit 0 ;;
    *) printf '%s\n' "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$output" ] || { usage >&2; exit 2; }
if [ -n "$libusb_source_dir" ]; then
    libusb_source_dir=$(CDPATH='' cd -- "$libusb_source_dir" && pwd)
    if [ ! -x "$libusb_source_dir/configure" ] || [ ! -f "$libusb_source_dir/COPYING" ]; then
        printf '%s\n' "invalid libusb source directory: $libusb_source_dir" >&2
        exit 1
    fi
fi

# Pinned inputs. Update together with the release record.
llvm_mingw_version=20250910
llvm_mingw_archive="llvm-mingw-${llvm_mingw_version}-ucrt-ubuntu-22.04-x86_64.tar.xz"
llvm_mingw_sha256=f83556c9ffa4d4291fadea1a0776c1383332dacdf4d7fbdf974c2928cb32c6f7
llvm_mingw_url="https://github.com/mstorsjo/llvm-mingw/releases/download/${llvm_mingw_version}/${llvm_mingw_archive}"

libusb_version=1.0.30
libusb_archive="libusb-${libusb_version}.tar.bz2"
libusb_sha256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
libusb_url="https://github.com/libusb/libusb/releases/download/v${libusb_version}/${libusb_archive}"

command -v cmake >/dev/null || { printf '%s\n' 'cmake is required' >&2; exit 1; }
command -v sha256sum >/dev/null || { printf '%s\n' 'sha256sum is required' >&2; exit 1; }
command -v make >/dev/null || { printf '%s\n' 'make is required' >&2; exit 1; }
command -v strings >/dev/null || { printf '%s\n' 'strings is required' >&2; exit 1; }
if command -v ninja >/dev/null; then make_program=$(command -v ninja)
elif command -v samurai >/dev/null; then make_program=$(command -v samurai)
elif command -v samu >/dev/null; then make_program=$(command -v samu)
else printf '%s\n' 'ninja or samurai is required' >&2; exit 1
fi

mkdir -p "$output" "$cache"
output=$(CDPATH='' cd -- "$output" && pwd)
cache=$(CDPATH='' cd -- "$cache" && pwd)
[ -z "$test_output" ] || { mkdir -p "$test_output"; test_output=$(CDPATH='' cd -- "$test_output" && pwd); }

work=$(mktemp -d /tmp/asicen-windows.XXXXXX)
cleanup() { find "$work" -depth -delete; }
trap cleanup EXIT HUP INT TERM

# The pinned archive is verified on every run, and the compiler tree is always
# freshly extracted from that verified archive into this task's workspace. The
# cache therefore only holds the archive; a stale or tampered extracted tree in
# the cache is never trusted.
llvm_archive="$cache/$llvm_mingw_archive"
if [ ! -f "$llvm_archive" ]; then
    [ "$skip_toolchain_download" -eq 0 ] || {
        printf '%s\n' "toolchain archive not present at $llvm_archive; cannot verify the pinned toolchain" >&2
        exit 1
    }
    command -v curl >/dev/null || { printf '%s\n' 'curl is required to fetch the toolchain' >&2; exit 1; }
    curl -fsSL --retry 2 -o "$llvm_archive" "$llvm_mingw_url"
fi
actual=$(sha256sum "$llvm_archive" | awk '{print $1}')
[ "$actual" = "$llvm_mingw_sha256" ] || {
    printf '%s\n' "llvm-mingw checksum mismatch: $actual" >&2
    exit 1
}
mkdir -p "$work/toolchain"
tar -xf "$llvm_archive" -C "$work/toolchain"
toolchain_dir="$work/toolchain/llvm-mingw-${llvm_mingw_version}-ucrt-ubuntu-22.04-x86_64"
[ -d "$toolchain_dir" ] || {
    printf '%s\n' 'llvm-mingw archive did not expand as expected' >&2
    exit 1
}

# The narrow llvm-mingw runtime license texts shipped in the Windows archive
# must be the exact texts of this checksum-verified toolchain. Compare the
# tracked copies against the freshly extracted archive and fail on any drift.
licenses_root="$root/packaging/windows-toolchain/licenses"
for license in \
    LICENSE.TXT \
    mingw32/COPYING \
    mingw32/COPYING.MinGW-w64-runtime.txt \
    mingw32/COPYING.MinGW-w64.txt \
    mingw32/COPYING.winpthreads.txt \
    mingw32/COPYING.winstorecompat.txt; do
    case "$license" in
    LICENSE.TXT) extracted="$toolchain_dir/LICENSE.TXT" ;;
    *) extracted="$toolchain_dir/x86_64-w64-mingw32/share/$license" ;;
    esac
    [ -f "$licenses_root/$license" ] || {
        printf '%s\n' "tracked toolchain license is missing: $licenses_root/$license" >&2
        exit 1
    }
    cmp -s "$licenses_root/$license" "$extracted" || {
        printf '%s\n' "tracked toolchain license differs from the verified toolchain: $license" >&2
        exit 1
    }
done

toolchain_bin="$toolchain_dir/bin"
[ -x "$toolchain_bin/x86_64-w64-mingw32-clang++" ] || {
    printf '%s\n' "llvm-mingw C++ compiler missing under $toolchain_bin" >&2
    exit 1
}
PATH="$toolchain_bin:$PATH"
export PATH

# Build a static libusb with the same pinned toolchain and link it into asicend.
# libusb 1.0.30's public header declares LIBUSB_CALL without dllimport, so
# consumers do not define LIBUSB_STATIC. Its Windows backend LoadLibrary's
# WinUSB, SetupAPI, Cfgmgr32, AdvAPI32, and hid.dll; configure.ac adds none of
# those to LIBS. The installed pkg-config Libs.private is passed through.
# --libusb-source-dir is the LGPL relink path and skips the archive checksum.
# The production path still requires the pinned archive.
mkdir -p "$work/libusb"
if [ -n "$libusb_source_dir" ]; then
    cp -a "$libusb_source_dir/." "$work/libusb/"
else
    libusb_archive_path="$cache/$libusb_archive"
    if [ ! -f "$libusb_archive_path" ]; then
        [ "$skip_toolchain_download" -eq 0 ] || {
            printf '%s\n' "libusb archive not present at $libusb_archive_path" >&2
            exit 1
        }
        command -v curl >/dev/null || { printf '%s\n' 'curl is required to fetch libusb' >&2; exit 1; }
        curl -fsSL --retry 2 -o "$libusb_archive_path" "$libusb_url"
    fi
    actual=$(sha256sum "$libusb_archive_path" | awk '{print $1}')
    [ "$actual" = "$libusb_sha256" ] || {
        printf '%s\n' "libusb checksum mismatch: $actual" >&2
        exit 1
    }
    tar -xjf "$libusb_archive_path" -C "$work/libusb" --strip-components=1
fi
[ -x "$work/libusb/configure" ] || {
    printf '%s\n' 'libusb source does not contain a configure script' >&2
    exit 1
}
(
    cd "$work/libusb" &&
        ./configure --host=x86_64-w64-mingw32 --prefix="$work/libusb-prefix" \
            --enable-static --disable-shared --disable-udev \
            --disable-examples-build --disable-tests-build \
            CC="$toolchain_bin/x86_64-w64-mingw32-clang" \
            AR="$toolchain_bin/llvm-ar" RANLIB="$toolchain_bin/llvm-ranlib" \
            CFLAGS="-O2 -g0 -ffile-prefix-map=$work=. -fdebug-prefix-map=$work=. -fmacro-prefix-map=$work=." \
            LDFLAGS="-Wl,-s -Wl,--no-insert-timestamp -Wl,--build-id=none" &&
        make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" &&
        make install
)
libusb_include="$work/libusb-prefix/include/libusb-1.0"
libusb_a="$work/libusb-prefix/lib/libusb-1.0.a"
libusb_pc="$work/libusb-prefix/lib/pkgconfig/libusb-1.0.pc"
if [ ! -f "$libusb_a" ] || [ ! -f "$libusb_include/libusb.h" ] || [ ! -f "$libusb_pc" ]; then
    printf '%s\n' 'static libusb archive was not produced' >&2
    exit 1
fi
if [ -e "$work/libusb-prefix/bin/libusb-1.0.dll" ] ||
    [ -e "$work/libusb-prefix/lib/libusb-1.0.dll" ] ||
    [ -e "$work/libusb-prefix/lib/libusb-1.0.dll.a" ]; then
    printf '%s\n' 'static libusb configure produced a shared DLL or import library' >&2
    exit 1
fi
nm_tool="$toolchain_bin/llvm-nm"
objdump_tool="$toolchain_bin/llvm-objdump"
if [ ! -x "$nm_tool" ] || [ ! -x "$objdump_tool" ]; then
    printf '%s\n' "llvm-nm and llvm-objdump are required under $toolchain_bin" >&2
    exit 1
fi
"$nm_tool" "$libusb_a" | grep -F 'libusb_init' >/dev/null || {
    printf '%s\n' 'static libusb archive is missing libusb_init' >&2
    exit 1
}
libusb_private=$(awk '
    $0 ~ /^Libs\.private:/ {
        found = 1
        sub(/^Libs\.private:[[:space:]]*/, "")
        print
        exit
    }
    END { if (!found) exit 1 }
' "$libusb_pc") || {
    printf '%s\n' 'libusb-1.0.pc has no Libs.private field' >&2
    exit 1
}
libusb_private=$(printf '%s' "$libusb_private" | awk '{$1=$1; print}')
printf '%s\n' "libusb Libs.private: ${libusb_private:-<empty>}"

configure_build() {
    build_dir=$1
    tests=$2
    libusb=$3
    link_arg=
    if [ "$libusb" = ON ] && [ -n "$libusb_private" ]; then
        link_arg="-DASICEN_LIBUSB_LINK_LIBRARIES=${libusb_private}"
    fi
    cmake -S "$root" -B "$build_dir" -G Ninja -DCMAKE_MAKE_PROGRAM="$make_program" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
        -DCMAKE_C_COMPILER="$toolchain_bin/x86_64-w64-mingw32-clang" \
        -DCMAKE_CXX_COMPILER="$toolchain_bin/x86_64-w64-mingw32-clang++" \
        -DCMAKE_RC_COMPILER="$toolchain_bin/llvm-rc" \
        -DCMAKE_EXE_LINKER_FLAGS="-Wl,-s" \
        -DASICEN_BUILD_TESTS="$tests" -DASICEN_BUILD_PCSC_IFD=OFF -DASICEN_ENABLE_LIBUSB="$libusb" \
        -DASICEN_LIBUSB_INCLUDE_DIR="$libusb_include" \
        -DASICEN_LIBUSB_LIBRARY="$libusb_a" \
        ${link_arg:+"$link_arg"}
}

build="$work/build"
configure_build "$build" OFF ON
cmake --build "$build" --target asicend asicen-ts asicenctl

for program in asicend asicen-ts asicenctl; do
    [ -f "$build/$program.exe" ] || {
        printf '%s\n' "expected executable $program.exe was not produced" >&2
        exit 1
    }
done
for program in asicend asicen-ts asicenctl; do
    cp "$build/$program.exe" "$output/$program.exe"
done
if [ -e "$output/libusb-1.0.dll" ]; then
    printf '%s\n' 'refusing to stage libusb-1.0.dll; libusb is statically linked' >&2
    exit 1
fi
for program in asicend asicen-ts asicenctl; do
    if "$objdump_tool" -p "$output/$program.exe" | grep -F 'libusb-1.0.dll' >/dev/null; then
        printf '%s\n' "$program.exe imports libusb-1.0.dll" >&2
        exit 1
    fi
done
# The pinned archive compiles the libusb version URL into asicend. A relink from
# --libusb-source-dir may replace that URL, so only the production path checks it.
if [ -z "$libusb_source_dir" ]; then
    if ! strings "$output/asicend.exe" | grep -F 'https://libusb.info' >/dev/null; then
        printf '%s\n' 'asicend.exe does not contain the static libusb version URL' >&2
        exit 1
    fi
    for program in asicen-ts asicenctl; do
        if strings "$output/$program.exe" | grep -F 'https://libusb.info' >/dev/null; then
            printf '%s\n' "$program.exe contains the libusb version URL" >&2
            exit 1
        fi
    done
fi

if [ -n "$test_output" ]; then
    test_build="$work/build-tests"
    # Include the hardware-free RAW_IO policy suite against the same pinned
    # libusb as production. No test opens a physical USB device.
    configure_build "$test_build" ON ON
    cmake --build "$test_build"
    for test_program in "$test_build"/*_tests.exe "$test_build"/*-tests.exe; do
        [ -f "$test_program" ] || continue
        cp "$test_program" "$test_output/"
    done
    [ -f "$test_output/asicen_windows_tests.exe" ] || {
        printf '%s\n' 'Windows test executables were not produced' >&2
        exit 1
    }
    [ -f "$test_output/asicen_windows_libusb_stream_policy_tests.exe" ] || {
        printf '%s\n' 'Windows libusb stream policy tests were not produced' >&2
        exit 1
    }
    if [ -e "$test_output/libusb-1.0.dll" ]; then
        printf '%s\n' 'refusing to stage libusb-1.0.dll next to the tests' >&2
        exit 1
    fi
fi

printf '%s\n' "Windows Phase 1 build staged in $output"
[ -z "$test_output" ] || printf '%s\n' "Windows Phase 1 tests staged in $test_output"
