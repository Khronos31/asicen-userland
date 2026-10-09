#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Prove the Android modified-libusb relink path. build-android.sh keeps the
# official libusb SHA-256 pin. This script unpacks that archive, changes
# LIBUSB_NANO, builds a static library with the NDK clang, and links asicend
# through -DASICEN_LIBUSB_LIBRARY. It does not replace the release pin.
set -eu
umask 022

abi=x86_64
libusb_archive=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --abi) abi=$2; shift 2 ;;
        --libusb-archive) libusb_archive=$2; shift 2 ;;
        --help)
            printf '%s\n' "usage: $0 [--abi x86_64|aarch64|armv7a] [--libusb-archive FILE]"
            exit 0
            ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; exit 2 ;;
    esac
done

case "$abi" in
    aarch64) ndk_abi=arm64-v8a; triple=aarch64-linux-android24; host=aarch64-linux-android ;;
    armv7a) ndk_abi=armeabi-v7a; triple=armv7a-linux-androideabi24; host=armv7a-linux-androideabi ;;
    x86_64) ndk_abi=x86_64; triple=x86_64-linux-android24; host=x86_64-linux-android ;;
    *) printf 'unsupported ABI: %s\n' "$abi" >&2; exit 2 ;;
esac

ndk=${ANDROID_NDK_HOME:-}
[ -n "$ndk" ] && [ -d "$ndk" ] || { printf '%s\n' 'ANDROID_NDK_HOME is required' >&2; exit 1; }
ndk=$(cd -- "$ndk" && pwd -P)
case "$(uname -s)-$(uname -m)" in
    Linux-x86_64) prebuilt=linux-x86_64 ;;
    Linux-aarch64) prebuilt=linux-aarch64 ;;
    *) printf 'unsupported relink host: %s %s\n' "$(uname -s)" "$(uname -m)" >&2; exit 1 ;;
esac
toolchain=$ndk/toolchains/llvm/prebuilt/$prebuilt
cc=$toolchain/bin/${triple}-clang
cxx=$toolchain/bin/${triple}-clang++
ar=$toolchain/bin/llvm-ar
ranlib=$toolchain/bin/llvm-ranlib
nm=$toolchain/bin/llvm-nm
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
work=$(mktemp -d /tmp/asicen-android-relink.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

if [ -z "$libusb_archive" ]; then
    libusb_archive=$work/libusb-1.0.30.tar.bz2
    curl -fsSL --retry 2 -o "$libusb_archive" \
        https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
fi
echo 'fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf  '"$libusb_archive" | sha256sum -c -
tar -xjf "$libusb_archive" -C "$work"
python3 - "$work/libusb-1.0.30/libusb/version_nano.h" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = "#define LIBUSB_NANO 12037"
if text.count(old) != 1:
    raise SystemExit("pinned libusb version_nano.h did not match")
path.write_text(text.replace(old, "#define LIBUSB_NANO 12038", 1), encoding="utf-8")
PY

prefix=$work/prefix
mkdir -p "$prefix" "$work/tmp"
(
    cd "$work/libusb-1.0.30"
    CC="$cc" CXX="$cxx" AR="$ar" RANLIB="$ranlib" \
        CFLAGS="-O2 -fPIC" LDFLAGS="-fPIC" PKG_CONFIG=/bin/false \
        ./configure --host="$host" --prefix="$prefix" --libdir="$prefix/lib" \
            --disable-shared --enable-static --with-pic --disable-udev \
            --disable-examples-build --disable-tests-build --disable-dependency-tracking
    make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
    make install
)
python3 - "$prefix/lib/libusb-1.0.a" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
old = (12037).to_bytes(4, "little")
new = (12038).to_bytes(4, "little")
if old in data:
    raise SystemExit("modified libusb archive still contains LIBUSB_NANO 12037")
if new not in data:
    raise SystemExit("modified libusb archive does not contain LIBUSB_NANO 12038")
PY

cmake=$(command -v cmake)
ninja=$(command -v ninja)
"$cmake" -S "$root" -B "$work/build" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$ndk/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ndk_abi" \
    -DANDROID_PLATFORM=android-24 \
    -DCMAKE_ANDROID_STL_TYPE=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DASICEN_ANDROID_NDK_ROOT_MAP="$ndk" \
    -DASICEN_ENABLE_LIBUSB=ON \
    -DASICEN_ENABLE_IFD=OFF \
    -DASICEN_BUILD_TESTS=OFF \
    -DASICEN_LIBUSB_INCLUDE_DIR="$prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$prefix/lib/libusb-1.0.a"
"$cmake" --build "$work/build" --target asicend -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
"$nm" "$work/build/asicend" | grep -q 'libusb_init' || {
    printf '%s\n' 'relinked asicend does not contain libusb_init' >&2
    exit 1
}
python3 - "$work/build/asicend" <<'PY'
from pathlib import Path
import sys
data = Path(sys.argv[1]).read_bytes()
new = (12038).to_bytes(4, "little")
if new not in data:
    raise SystemExit("relinked asicend does not contain modified LIBUSB_NANO 12038")
PY
printf '%s\n' "android modified-libusb relink: PASS ($abi)"
