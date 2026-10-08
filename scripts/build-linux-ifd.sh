#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
umask 022

usage() {
    printf '%s\n' 'usage: build-linux-ifd.sh --libc glibc|musl --source-snapshot FILE.tar.gz --gcc-copying3 FILE --gcc-runtime-exception FILE [--musl-license FILE] --output DIR'
}
libc=
source_snapshot=
gcc_copying3=
gcc_runtime_exception=
musl_license=
output=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --libc) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; libc=$2; shift 2 ;;
        --source-snapshot) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_snapshot=$2; shift 2 ;;
        --gcc-copying3) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; gcc_copying3=$2; shift 2 ;;
        --gcc-runtime-exception) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; gcc_runtime_exception=$2; shift 2 ;;
        --musl-license) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; musl_license=$2; shift 2 ;;
        --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
        --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
case "$libc" in glibc|musl) ;; *) usage >&2; exit 2 ;; esac
[ -n "$source_snapshot" ] && [ -n "$gcc_copying3" ] && \
    [ -n "$gcc_runtime_exception" ] && [ -n "$output" ] || { usage >&2; exit 2; }
if [ "$libc" = musl ]; then
    [ -n "$musl_license" ] || { usage >&2; exit 2; }
fi

source_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
for tool in cmake ninja gcc g++ nm strip readelf sha256sum tar realpath python3; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'required tool missing: %s\n' "$tool" >&2; exit 1; }
done
source_snapshot=$(realpath -e "$source_snapshot")
gcc_copying3=$(realpath -e "$gcc_copying3")
gcc_runtime_exception=$(realpath -e "$gcc_runtime_exception")
for path in "$gcc_copying3" "$gcc_runtime_exception"; do
    [ -f "$path" ] || { printf 'toolchain license is missing: %s\n' "$path" >&2; exit 1; }
done
gcc_copying3_sha=$(sha256sum "$gcc_copying3" | awk '{print $1}')
gcc_runtime_exception_sha=$(sha256sum "$gcc_runtime_exception" | awk '{print $1}')
[ "$gcc_copying3_sha" = 8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903 ] || {
    printf 'unexpected GCC COPYING3 SHA-256: %s\n' "$gcc_copying3_sha" >&2; exit 1;
}
[ "$gcc_runtime_exception_sha" = 9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74 ] || {
    printf 'unexpected GCC runtime exception SHA-256: %s\n' "$gcc_runtime_exception_sha" >&2; exit 1;
}
if [ "$libc" = glibc ]; then
    libc_version=$(getconf GNU_LIBC_VERSION 2>/dev/null || true)
    [ "$libc_version" = 'glibc 2.31' ] || {
        printf 'glibc IFD requires a 2.31 build root; detected %s\n' "${libc_version:-unknown}" >&2; exit 1;
    }
    gcc_version=$(g++ -dumpfullversion -dumpversion)
    [ "$gcc_version" = 10.2.1 ] || { printf 'expected GCC 10.2.1, found %s\n' "$gcc_version" >&2; exit 1; }
    libc_archive=$(realpath -e "$(gcc -print-file-name=libc.a)")
    libc_version_identity=glibc-2.31
else
    target=$(gcc -dumpmachine)
    case "$target" in *linux-musl*) ;; *) printf 'musl IFD requires a musl target compiler; detected %s\n' "$target" >&2; exit 1 ;; esac
    gcc_version=$(g++ -dumpfullversion -dumpversion)
    [ "$gcc_version" = 14.2.0 ] || { printf 'expected GCC 14.2.0, found %s\n' "$gcc_version" >&2; exit 1; }
    [ -n "$musl_license" ] || { usage >&2; exit 2; }
    musl_license=$(realpath -e "$musl_license")
    [ -f "$musl_license" ] && [ ! -L "$musl_license" ] || { printf '%s\n' 'musl license must be a regular file' >&2; exit 1; }
    musl_license_sha=$(sha256sum "$musl_license" | awk '{print $1}')
    [ "$musl_license_sha" = f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af ] || {
        printf 'unexpected musl license SHA-256: %s\n' "$musl_license_sha" >&2; exit 1;
    }
    libc_archive=$(realpath -e "$(gcc -print-file-name=libc.a)")
    libc_loader=$(realpath -e "$(gcc -print-file-name=libc.so)")
    loader_output=$("$libc_loader" 2>&1 || true)
    printf '%s\n' "$loader_output" | grep -E '^musl libc \(' >/dev/null || {
        printf '%s\n' 'compiler-selected musl loader identity check failed' >&2; exit 1;
    }
    libc_version_identity=musl
fi
libc_archive_sha=$(sha256sum "$libc_archive" | awk '{print $1}')
gcc_bin=$(realpath -e "$(command -v gcc)")
gxx_bin=$(realpath -e "$(command -v g++)")
gcc_sha=$(sha256sum "$gcc_bin" | awk '{print $1}')
gxx_sha=$(sha256sum "$gxx_bin" | awk '{print $1}')
source_sha=$(sha256sum "$source_snapshot" | awk '{print $1}')
python3 "$source_root/scripts/audit-linux-candidate.py" --source-archive "$source_snapshot"

parent=$(dirname -- "$output")
mkdir -p "$parent"
parent=$(CDPATH='' cd -- "$parent" && pwd -P)
output="$parent/$(basename -- "$output")"
case "$source_snapshot" in "$source_root"/*) printf '%s\n' 'source snapshot must be outside mutable checkout' >&2; exit 1 ;; esac
case "$output/" in "$source_root/"*) printf '%s\n' 'output must be outside source tree' >&2; exit 1 ;; esac
[ ! -e "$output" ] && [ ! -L "$output" ] || { printf 'output already exists or is a symlink: %s\n' "$output" >&2; exit 1; }
work=$(mktemp -d "$parent/.asicen-ifd-work.XXXXXX")
stage=$(mktemp -d "$parent/.asicen-ifd-stage.XXXXXX")
cleanup() { rm -rf -- "$work" "$stage"; }
trap cleanup EXIT HUP INT TERM
mkdir -p "$work/src" "$stage/licenses"
tar -xzf "$source_snapshot" -C "$work/src"

cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DASICEN_ENABLE_LIBUSB=OFF -DASICEN_ENABLE_IFD=ON -DASICEN_REQUIRE_IFD=ON \
    -DASICEN_BUILD_TESTS=ON -DCMAKE_CXX_FLAGS='-fPIC -static-libstdc++ -static-libgcc' \
    -DCMAKE_SHARED_LINKER_FLAGS='-static-libstdc++ -static-libgcc -Wl,--build-id=none'
cmake --build "$work/build" --target asicen-ifdhandler asicen-ifd-prefix-tests px4-pcsc-ifd-tests --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
(cd "$work/build" && ctest -R 'pcsc-ifd|ifd-prefix' --output-on-failure --no-tests=error)
library="$work/build/libifd-asicen.so"
[ -f "$library" ] || { printf 'expected IFD library not found: %s\n' "$library" >&2; exit 1; }

verify_exports() {
    symbols=$(nm -D --defined-only "$1")
    for symbol in IFDHCreateChannel IFDHCreateChannelByName IFDHCloseChannel \
        IFDHGetCapabilities IFDHSetCapabilities IFDHSetProtocolParameters \
        IFDHPowerICC IFDHTransmitToICC IFDHControl IFDHICCPresence; do
        printf '%s\n' "$symbols" | grep -E "[[:space:]]_?$symbol$" >/dev/null || {
            printf 'missing IFD export: %s\n' "$symbol" >&2; return 1;
        }
    done
}
verify_exports "$library"
strip --strip-unneeded "$library"
verify_exports "$library"
cp "$library" "$stage/libifd-asicen.so"
cp "$gcc_copying3" "$stage/licenses/GCC-COPYING3.txt"
cp "$gcc_runtime_exception" "$stage/licenses/GCC-COPYING.RUNTIME.txt"
arch=$(readelf -h "$stage/libifd-asicen.so" | awk -F: '/Machine:/ {sub(/^[[:space:]]+/, "", $2); print $2}')
case "$libc" in
    glibc)
        versions=$(readelf --version-info "$stage/libifd-asicen.so" | grep -o 'GLIBC_[0-9][0-9.]*' | sort -Vu || true)
        too_new=$(printf '%s\n' "$versions" | awk -F_ '{split($2,a,"."); if (a[1]>2 || (a[1]==2 && a[2]>31)) print $0}')
        [ -z "$too_new" ] || { printf 'IFD requires newer glibc symbols: %s\n' "$too_new" >&2; exit 1; }
        ;;
    musl)
        versions=$(readelf --version-info "$stage/libifd-asicen.so" | grep -o 'GLIBC_[0-9][0-9.]*' || true)
        [ -z "$versions" ] || { printf 'musl IFD unexpectedly requires glibc: %s\n' "$versions" >&2; exit 1; }
        cp "$musl_license" "$stage/licenses/musl-COPYRIGHT.txt"
        ;;
esac
cat > "$stage/BUILD-METADATA.txt" <<EOF
Artifact: host-loaded PC/SC IFD shared plugin
Libc variant: $libc_version_identity
Architecture: $arch
Source archive SHA-256: $source_sha
Compiler target: $(gcc -dumpmachine)
Compiler version: $gcc_version
GCC executable SHA-256: $gcc_sha
G++ executable SHA-256: $gxx_sha
libc archive path: $libc_archive
libc archive SHA-256: $libc_archive_sha
GCC COPYING3 SHA-256: $gcc_copying3_sha
GCC runtime exception SHA-256: $gcc_runtime_exception_sha
EOF
if [ "$libc" = musl ]; then
    cat >> "$stage/BUILD-METADATA.txt" <<EOF
Musl loader: $libc_loader
Musl loader identification: $(printf '%s\n' "$loader_output" | grep -E -m 1 '^musl libc \(')
Musl loader SHA-256: $(sha256sum "$libc_loader" | awk '{print $1}')
Musl license SHA-256: $musl_license_sha
EOF
fi
(
    cd "$stage"
    sha256sum libifd-asicen.so BUILD-METADATA.txt licenses/* > SHA256SUMS
)
chmod 0755 "$stage" "$stage/licenses" "$stage/libifd-asicen.so"
chmod 0644 "$stage/BUILD-METADATA.txt" "$stage/SHA256SUMS" "$stage/licenses/"*
mv -- "$stage" "$output"
rm -rf -- "$work"
trap - EXIT HUP INT TERM
printf 'IFD candidate created: %s\n' "$output"
