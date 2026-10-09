#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build the Android API-24 command-line artifacts with isolated static libusb.
set -eu
umask 022

api=24
abi=
output=
evidence=
libusb_archive_input=

usage()
{
    printf '%s\n' \
        "usage: $0 --abi aarch64|arm64-v8a|armv7a|armeabi-v7a|x86_64 --output DIR" \
        "          [--evidence DIR] [--libusb-archive FILE]"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
    --abi)
        [ "$#" -ge 2 ] || { usage >&2; exit 2; }
        abi=$2
        shift 2
        ;;
    --output)
        [ "$#" -ge 2 ] || { usage >&2; exit 2; }
        output=$2
        shift 2
        ;;
    --evidence)
        [ "$#" -ge 2 ] || { usage >&2; exit 2; }
        evidence=$2
        shift 2
        ;;
    --libusb-archive)
        [ "$#" -ge 2 ] || { usage >&2; exit 2; }
        libusb_archive_input=$2
        shift 2
        ;;
    --help)
        usage
        exit 0
        ;;
    *)
        printf '%s\n' "unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
    esac
done

[ -n "$output" ] || { printf '%s\n' '--output is required' >&2; exit 2; }
case "$abi" in
arm64-v8a|aarch64)
    abi=aarch64
    ndk_abi=arm64-v8a
    clang_triple=aarch64-linux-android${api}
    autotools_host=aarch64-linux-android
    expected_interpreter=/system/bin/linker64
    expected_arch=aarch64
    ;;
armeabi-v7a|armv7a)
    abi=armv7a
    ndk_abi=armeabi-v7a
    clang_triple=armv7a-linux-androideabi${api}
    autotools_host=armv7a-linux-androideabi
    expected_interpreter=/system/bin/linker
    expected_arch=armv7a
    ;;
x86_64)
    abi=x86_64
    ndk_abi=x86_64
    clang_triple=x86_64-linux-android${api}
    autotools_host=x86_64-linux-android
    expected_interpreter=/system/bin/linker64
    expected_arch=x86_64
    ;;
*)
    printf '%s\n' "unsupported ABI: $abi" >&2
    exit 2
    ;;
esac

ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
[ -n "$ndk" ] && [ -d "$ndk" ] || {
    printf '%s\n' 'ANDROID_NDK_HOME must point to an Android NDK' >&2
    exit 1
}
ndk=$(cd -- "$ndk" && pwd -P)
revision=$(sed -n 's/^Pkg.Revision = //p' "$ndk/source.properties" | head -n 1)
major=${revision%%.*}
case "$major" in
''|*[!0-9]*)
    printf '%s\n' "cannot determine NDK revision: $ndk/source.properties" >&2
    exit 1
    ;;
esac
if [ -n "$evidence" ] && [ "$major" -ne 27 ]; then
    printf '%s\n' "release evidence requires NDK r27, got $revision" >&2
    exit 1
fi

case "$(uname -s)-$(uname -m)" in
Linux-x86_64)
    prebuilt=linux-x86_64
    ;;
Linux-aarch64)
    prebuilt=linux-aarch64
    ;;
Darwin-arm64)
    prebuilt=darwin-arm64
    ;;
Darwin-x86_64)
    prebuilt=darwin-x86_64
    ;;
*)
    printf '%s\n' "unsupported build host: $(uname -s) $(uname -m)" >&2
    exit 1
    ;;
esac

toolchain=$ndk/toolchains/llvm/prebuilt/$prebuilt
cc=$toolchain/bin/${clang_triple}-clang
cxx=$toolchain/bin/${clang_triple}-clang++
ar=$toolchain/bin/llvm-ar
ranlib=$toolchain/bin/llvm-ranlib
strip=$toolchain/bin/llvm-strip
nm=$toolchain/bin/llvm-nm
for tool in "$cc" "$cxx" "$ar" "$ranlib" "$strip" "$nm"; do
    [ -x "$tool" ] || { printf '%s\n' "missing NDK tool: $tool" >&2; exit 1; }
done

root=$(cd -- "$(dirname -- "$0")/.." && pwd -P)
cmake_bin=$(command -v cmake)
ninja_bin=$(command -v ninja)
[ -x "$cmake_bin" ] || { printf '%s\n' 'cmake is required' >&2; exit 1; }
[ -x "$ninja_bin" ] || { printf '%s\n' 'ninja is required' >&2; exit 1; }
host_path=$(dirname "$cmake_bin"):$(dirname "$ninja_bin"):/usr/bin:/bin
if [ -d "$output" ]; then
    output=$(cd -- "$output" && pwd)
else
    mkdir -p "$output"
    output=$(cd -- "$output" && pwd)
fi
if [ -n "$evidence" ]; then
    case "$evidence" in
    /*) ;;
    *) evidence=$(pwd)/$evidence ;;
    esac
    evidence_parent=$(dirname -- "$evidence")
    mkdir -p "$evidence_parent"
    evidence_parent=$(cd -- "$evidence_parent" && pwd)
    evidence=$evidence_parent/$(basename -- "$evidence")
    [ ! -e "$evidence" ] && [ ! -L "$evidence" ] || {
        printf '%s\n' "evidence destination already exists: $evidence" >&2
        exit 1
    }
fi

work=$(mktemp -d /tmp/asicen-userland-android.XXXXXX)
evidence_tmp=
cleanup()
{
    if [ -d "$work" ]; then
        find "$work" -depth -delete
    fi
    if [ -n "$evidence_tmp" ] && [ -d "$evidence_tmp" ]; then
        find "$evidence_tmp" -depth -delete
    fi
}
trap cleanup EXIT HUP INT TERM
prefix=$work/prefix
archive=$work/libusb-1.0.30.tar.bz2
libusb_source=$work/libusb-1.0.30
cmake_build=$work/cmake
output_daemon=$output/asicend-$abi
output_control=$output/asicenctl-$abi
output_stream=$output/asicen-ts-$abi
mkdir -p "$prefix" "$work/tmp"

# libusb is built outside CMake, so give its compiler the same reproducibility
# contract as the project build.  Map the complete temporary work tree because
# configure/build directories can otherwise enter DWARF as compilation paths.
libusb_prefix_maps="-fdebug-compilation-dir=."
for prefix_map in "$libusb_source" "$work" "$ndk"; do
    libusb_prefix_maps="$libusb_prefix_maps -ffile-prefix-map=$prefix_map=."
    libusb_prefix_maps="$libusb_prefix_maps -fdebug-prefix-map=$prefix_map=."
    libusb_prefix_maps="$libusb_prefix_maps -fmacro-prefix-map=$prefix_map=."
done

libusb_url=https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
libusb_sha256=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
if [ -n "$libusb_archive_input" ]; then
    [ -f "$libusb_archive_input" ] && [ ! -L "$libusb_archive_input" ] || {
        printf '%s\n' "libusb archive not found: $libusb_archive_input" >&2
        exit 1
    }
    archive=$(cd -- "$(dirname -- "$libusb_archive_input")" && pwd -P)/$(basename -- "$libusb_archive_input")
else
    curl -fsSL --retry 2 -o "$archive" "$libusb_url"
fi
if command -v sha256sum >/dev/null 2>&1; then
    actual=$(sha256sum "$archive" | awk '{print $1}')
else
    actual=$(shasum -a 256 "$archive" | awk '{print $1}')
fi
[ "$actual" = "$libusb_sha256" ] || {
    printf '%s\n' "libusb checksum mismatch: $actual" >&2
    exit 1
}
tar -xjf "$archive" -C "$work"

jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '%s' 2)
env -i \
    PATH="$host_path" \
    TMPDIR="$work/tmp" \
    LC_ALL=C \
    CC="$cc" \
    CXX="$cxx" \
    AR="$ar" \
    RANLIB="$ranlib" \
    CFLAGS="-O2 -fPIC $libusb_prefix_maps" \
    LDFLAGS='-fPIC' \
    PKG_CONFIG=/bin/false \
    /bin/sh -c "
        set -eu
        cd \"$libusb_source\"
        ./configure \\
            --host=$autotools_host \\
            --prefix=\"$prefix\" \\
            --libdir=\"$prefix/lib\" \\
            --disable-shared \\
            --enable-static \\
            --with-pic \\
            --disable-udev \\
            --disable-examples-build \\
            --disable-tests-build \\
            --disable-dependency-tracking
        make -j$jobs
        make install
    "

[ -f "$prefix/lib/libusb-1.0.a" ] || {
    printf '%s\n' 'static libusb archive was not produced' >&2
    exit 1
}
if find "$prefix/lib" -maxdepth 1 -type f \( -name '*.so' -o -name '*.dylib' \) | grep -q .; then
    printf '%s\n' 'shared libusb output is forbidden' >&2
    exit 1
fi

ndk_cmake=$ndk/build/cmake/android.toolchain.cmake
env -i \
    PATH=/usr/bin:/bin \
    TMPDIR="$work/tmp" \
    LC_ALL=C \
    ANDROID_NDK_HOME="$ndk" \
    PKG_CONFIG=/bin/false \
    "$cmake_bin" -S "$root" -B "$cmake_build" \
        -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$ninja_bin" \
        -DCMAKE_TOOLCHAIN_FILE="$ndk_cmake" \
        -DANDROID_ABI="$ndk_abi" \
        -DANDROID_PLATFORM=android-$api \
        -DCMAKE_ANDROID_STL_TYPE=c++_static \
        -DCMAKE_BUILD_TYPE=Release \
        -DASICEN_ANDROID_NDK_ROOT_MAP="$ndk" \
        -DASICEN_ENABLE_LIBUSB=ON \
        -DASICEN_ENABLE_IFD=OFF \
        -DASICEN_BUILD_TESTS=OFF \
        -DASICEN_LIBUSB_INCLUDE_DIR="$prefix/include/libusb-1.0" \
        -DASICEN_LIBUSB_LIBRARY="$prefix/lib/libusb-1.0.a"
env -i \
    PATH="$host_path" \
    TMPDIR="$work/tmp" \
    LC_ALL=C \
    "$cmake_bin" --build "$cmake_build" \
        --target asicend asicenctl asicen-ts -j"$jobs"

# Record every command's symbol table before stripping.  The hardware command
# (asicend) is the only target that links the native transport, so its static
# libusb symbols must survive the final link before stripping.
nm_dir=$work/nm
mkdir -p "$nm_dir"
for binary in asicend asicenctl asicen-ts; do
    "$nm" "$cmake_build/$binary" >"$nm_dir/$binary.nm"
done
for symbol in libusb_init libusb_wrap_sys_device libusb_close; do
    grep -E "[[:space:]]$symbol$" "$nm_dir/asicend.nm" >/dev/null || {
        printf '%s\n' "missing static libusb symbol in asicend: $symbol" >&2
        exit 1
    }
done

# CMake/NDK can leave DWARF and the regular symbol table in a Release link.
# Strip only after the static-link checks, then verify the exact files that are
# copied to the publication directory.
for binary in asicend asicenctl asicen-ts; do
    "$strip" --strip-all "$cmake_build/$binary"
done
for binary in asicend asicenctl asicen-ts; do
    "$root/scripts/verify-android-elf.sh" \
        "$cmake_build/$binary" "$expected_interpreter" "$expected_arch"
done

if [ -n "$evidence" ]; then
    for required in "$ndk/source.properties" "$ndk/NOTICE" "$libusb_source/COPYING"; do
        [ -f "$required" ] || {
            printf '%s\n' "required release material missing: $required" >&2
            exit 1
        }
    done
    evidence_tmp=$(mktemp -d "$evidence.tmp.XXXXXX")
    mkdir -p "$evidence_tmp/libusb" "$evidence_tmp/ndk" "$evidence_tmp/nm"
    cp "$libusb_source/COPYING" "$evidence_tmp/libusb/COPYING"
    cp "$ndk/source.properties" "$evidence_tmp/ndk/source.properties"
    cp "$ndk/NOTICE" "$evidence_tmp/ndk/NOTICE"
    cp "$nm_dir/asicend.nm" "$nm_dir/asicenctl.nm" "$nm_dir/asicen-ts.nm" \
        "$evidence_tmp/nm/"
    {
        printf 'android_abi=%s\n' "$abi"
        printf 'android_api=%s\n' "$api"
        printf 'ndk_revision=%s\n' "$revision"
        printf 'libusb_version=1.0.30\n'
        printf 'libusb_archive_sha256=%s\n' "$libusb_sha256"
    } >"$evidence_tmp/build.properties"
fi

publish_daemon_tmp=$(mktemp "$output_daemon.tmp.XXXXXX")
publish_control_tmp=$(mktemp "$output_control.tmp.XXXXXX")
publish_stream_tmp=$(mktemp "$output_stream.tmp.XXXXXX")
cp "$cmake_build/asicend" "$publish_daemon_tmp"
cp "$cmake_build/asicenctl" "$publish_control_tmp"
cp "$cmake_build/asicen-ts" "$publish_stream_tmp"
chmod 0755 "$publish_daemon_tmp" "$publish_control_tmp" "$publish_stream_tmp"
mv -f "$publish_daemon_tmp" "$output_daemon"
mv -f "$publish_control_tmp" "$output_control"
mv -f "$publish_stream_tmp" "$output_stream"
if [ -n "$evidence_tmp" ]; then
    mv "$evidence_tmp" "$evidence"
    evidence_tmp=
fi
printf '%s\n' "built $output_daemon"
printf '%s\n' "built $output_control"
printf '%s\n' "built $output_stream"
[ -z "$evidence" ] || printf '%s\n' "built $evidence"
