#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu

candidate=${1:?usage: test-macos-libusb-relink.sh CANDIDATE_DIR}
candidate=$(cd "$candidate" && pwd -P)
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
export TMPDIR=/tmp
export MACOSX_DEPLOYMENT_TARGET=14.0
work=$(mktemp -d /tmp/asicen-libusb-relink.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

python3 "$repo/scripts/audit-macos-candidate.py" "$candidate"
mkdir -p "$work/project" "$work/libusb-src" "$work/libusb-prefix"
tar -xzf "$candidate/source/asicen-userland-source.tar.gz" -C "$work/project"
tar -xjf "$candidate/source/libusb-1.0.30.tar.bz2" -C "$work/libusb-src" --strip-components=1

python3 - "$work/libusb-src/libusb/version_nano.h" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
old = "#define LIBUSB_NANO 12037"
if text.count(old) != 1:
    raise SystemExit("pinned libusb version_nano.h did not match expected source")
path.write_text(text.replace(old, "#define LIBUSB_NANO 12038"), encoding="utf-8")
PY

(
    cd "$work/libusb-src"
    ./configure --prefix="$work/libusb-prefix" --disable-shared --enable-static --with-pic \
        --disable-udev --disable-examples-build --disable-tests-build --disable-dependency-tracking
    make -j"$(sysctl -n hw.logicalcpu 2>/dev/null || printf '2')"
    make install
)

xcrun clang -I"$work/libusb-prefix/include/libusb-1.0" \
    "$work/project/scripts/macos-libusb-version-smoke.c" \
    "$work/libusb-prefix/lib/libusb-1.0.a" \
    -framework IOKit -framework CoreFoundation -framework Security -lobjc \
    -o "$work/libusb-version-smoke"
"$work/libusb-version-smoke"

cmake -S "$work/project" -B "$work/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DCMAKE_SKIP_RPATH=ON \
    -DASICEN_ENABLE_LIBUSB=ON -DASICEN_ENABLE_IFD=ON -DASICEN_REQUIRE_IFD=ON \
    -DASICEN_BUILD_TESTS=OFF \
    -DASICEN_LIBUSB_INCLUDE_DIR="$work/libusb-prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$work/libusb-prefix/lib/libusb-1.0.a" \
    '-DASICEN_LIBUSB_EXTRA_LINK_OPTIONS=SHELL:-framework IOKit;SHELL:-framework CoreFoundation;SHELL:-framework Security;-lobjc'
cmake --build "$work/build" --parallel "$(sysctl -n hw.logicalcpu 2>/dev/null || printf '2')" \
    --target asicend asicen-ts asicenctl

for binary in asicend asicen-ts asicenctl; do
    "$work/build/$binary" --help >/dev/null
done
python3 "$work/project/userland/tests/product_cli_integration.py" \
    "$work/build/asicend" "$work/build/asicenctl" "$work/build/asicen-ts"
printf '%s\n' 'modified-libUSB source relink and product IPC smoke: PASS'
