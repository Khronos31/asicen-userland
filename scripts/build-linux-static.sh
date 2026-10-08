#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
umask 022

usage() {
    printf '%s\n' 'usage: build-linux-static.sh --output DIR --source-snapshot FILE.tar.gz --libusb-archive FILE --musl-license FILE --gcc-copying3 FILE --gcc-runtime-exception FILE'
}
output=
source_snapshot=
libusb_archive=
musl_license=
gcc_copying3=
gcc_runtime_exception=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; output=$2; shift 2 ;;
        --source-snapshot) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; source_snapshot=$2; shift 2 ;;
        --libusb-archive) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; libusb_archive=$2; shift 2 ;;
        --musl-license) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; musl_license=$2; shift 2 ;;
        --gcc-copying3) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; gcc_copying3=$2; shift 2 ;;
        --gcc-runtime-exception) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; gcc_runtime_exception=$2; shift 2 ;;
        --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$output" ] && [ -n "$source_snapshot" ] && [ -n "$libusb_archive" ] && \
    [ -n "$musl_license" ] && [ -n "$gcc_copying3" ] && [ -n "$gcc_runtime_exception" ] || { usage >&2; exit 2; }

source_root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
case "$(uname -m)" in x86_64|aarch64) ;; *) printf '%s\n' 'only Linux x86_64/aarch64 is supported by this script' >&2; exit 1 ;; esac
for tool in cmake ninja gcc g++ ar nm strip sha256sum tar gzip readelf strings realpath python3; do
    command -v "$tool" >/dev/null 2>&1 || { printf 'required tool missing: %s\n' "$tool" >&2; exit 1; }
done

target=$(gcc -dumpmachine)
case "$target" in *linux-musl*) ;; *) printf 'musl GCC target required; detected %s\n' "$target" >&2; exit 1 ;; esac
libc_archive=$(realpath -e "$(gcc -print-file-name=libc.a)")
[ -f "$libc_archive" ] || { printf 'compiler libc archive unavailable: %s\n' "$libc_archive" >&2; exit 1; }
libc_archive_sha=$(sha256sum "$libc_archive" | awk '{print $1}')
libc_loader=$(realpath -e "$(gcc -print-file-name=libc.so)")
loader_output=$("$libc_loader" 2>&1 || true)
printf '%s\n' "$loader_output" | grep -E '^musl libc \(' >/dev/null || {
    printf 'compiler-selected libc loader does not identify itself as musl: %s\n' "$libc_loader" >&2
    exit 1
}
libc_loader_sha=$(sha256sum "$libc_loader" | awk '{print $1}')

[ -f "$source_snapshot" ] && [ ! -L "$source_snapshot" ] || { printf '%s\n' 'source snapshot must be a regular non-symlink file' >&2; exit 1; }
[ -f "$libusb_archive" ] && [ ! -L "$libusb_archive" ] || { printf '%s\n' 'libusb archive must be a regular non-symlink file' >&2; exit 1; }
for license in "$musl_license" "$gcc_copying3" "$gcc_runtime_exception"; do
    [ -f "$license" ] && [ ! -L "$license" ] || { printf 'runtime license must be a regular non-symlink file: %s\n' "$license" >&2; exit 1; }
done
source_snapshot=$(realpath -e -- "$source_snapshot")
libusb_archive=$(realpath -e -- "$libusb_archive")
musl_license=$(realpath -e -- "$musl_license")
gcc_copying3=$(realpath -e -- "$gcc_copying3")
gcc_runtime_exception=$(realpath -e -- "$gcc_runtime_exception")
case "$source_snapshot" in "$source_root"/*) printf '%s\n' 'source snapshot must be outside the repository tree' >&2; exit 1 ;; esac
case "$libusb_archive" in "$source_root"/*) printf '%s\n' 'libusb source archive must be supplied from outside the source tree' >&2; exit 1 ;; esac
for license in "$musl_license" "$gcc_copying3" "$gcc_runtime_exception"; do
    case "$license" in "$source_root"/*) printf '%s\n' 'toolchain license must be supplied externally' >&2; exit 1 ;; esac
done
musl_license_sha=$(sha256sum "$musl_license" | awk '{print $1}')
gcc_copying3_sha=$(sha256sum "$gcc_copying3" | awk '{print $1}')
gcc_runtime_exception_sha=$(sha256sum "$gcc_runtime_exception" | awk '{print $1}')
[ "$musl_license_sha" = f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af ] || {
    printf 'unexpected musl 1.2.5 license text SHA-256: %s\n' "$musl_license_sha" >&2; exit 1;
}
[ "$gcc_copying3_sha" = 8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903 ] || {
    printf 'unexpected GCC COPYING3 SHA-256: %s\n' "$gcc_copying3_sha" >&2; exit 1;
}
[ "$gcc_runtime_exception_sha" = 9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74 ] || {
    printf 'unexpected GCC runtime exception SHA-256: %s\n' "$gcc_runtime_exception_sha" >&2; exit 1;
}
libusb_sha=$(sha256sum "$libusb_archive" | awk '{print $1}')
[ "$libusb_sha" = fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf ] || {
    printf 'unexpected libusb 1.0.30 source SHA-256: %s\n' "$libusb_sha" >&2; exit 1;
}
parent=$(dirname -- "$output")
mkdir -p "$parent"
parent=$(CDPATH='' cd -- "$parent" && pwd -P)
base=$(basename -- "$output")
output="$parent/$base"
case "$output/" in "$source_root/"*) printf '%s\n' 'output must be outside the source tree' >&2; exit 1 ;; esac
[ ! -e "$output" ] && [ ! -L "$output" ] || { printf 'output already exists or is a symlink: %s\n' "$output" >&2; exit 1; }
[ ! -e "$output.tar.gz" ] && [ ! -L "$output.tar.gz" ] || { printf 'candidate archive already exists: %s.tar.gz\n' "$output" >&2; exit 1; }
stage=$(mktemp -d "$parent/.asicen-static-stage.XXXXXX")
work=$(mktemp -d "$parent/.asicen-static-work.XXXXXX")
cleanup() { rm -rf -- "$stage" "$work"; }
trap cleanup EXIT HUP INT TERM

mkdir -p "$stage/source" "$stage/bin" "$stage/licenses" "$work/src" "$work/libusb-src" "$work/libusb-prefix"
# Build only from the source snapshot that is shipped with the candidate.
cp "$source_snapshot" "$stage/source/asicen-userland-source.tar.gz"
python3 "$source_root/scripts/audit-linux-candidate.py" \
    --source-archive "$stage/source/asicen-userland-source.tar.gz"
tar -xzf "$stage/source/asicen-userland-source.tar.gz" -C "$work/src"
source_sha=$(sha256sum "$stage/source/asicen-userland-source.tar.gz" | awk '{print $1}')

tar -xjf "$libusb_archive" -C "$work/libusb-src" --strip-components=1
(cd "$work/libusb-src" && ./configure --prefix="$work/libusb-prefix" --disable-shared \
    --enable-static --with-pic --disable-udev --disable-examples-build \
    --disable-tests-build --disable-dependency-tracking && \
    make -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" && make install)
[ -f "$work/libusb-prefix/lib/libusb-1.0.a" ] || { printf '%s\n' 'static libusb archive was not produced' >&2; exit 1; }
cp "$libusb_archive" "$stage/source/libusb-1.0.30.tar.bz2"
cp "$work/libusb-src/COPYING" "$stage/licenses/libusb-COPYING"
cp "$musl_license" "$stage/licenses/musl-COPYRIGHT.txt"
cp "$gcc_copying3" "$stage/licenses/GCC-COPYING3.txt"
cp "$gcc_runtime_exception" "$stage/licenses/GCC-COPYING.RUNTIME.txt"
cp "$work/src/COPYING.gpl2" "$stage/licenses/COPYING.gpl2"
cp "$work/src/third_party/px4-userland/LICENSE" "$stage/licenses/px4-userland-GPL-2.0-only.txt"
cp "$work/src/NOTICES.md" "$stage/licenses/NOTICES.md"

cmake -S "$work/src" -B "$work/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DASICEN_ENABLE_LIBUSB=ON -DASICEN_ENABLE_IFD=OFF -DASICEN_BUILD_TESTS=OFF \
    -DASICEN_LIBUSB_INCLUDE_DIR="$work/libusb-prefix/include/libusb-1.0" \
    -DASICEN_LIBUSB_LIBRARY="$work/libusb-prefix/lib/libusb-1.0.a" \
    -DASICEN_LIBUSB_EXTRA_LIBRARIES=pthread \
    -DCMAKE_CXX_FLAGS='-static -static-libstdc++ -static-libgcc' \
    -DCMAKE_EXE_LINKER_FLAGS="-static -static-libstdc++ -static-libgcc -Wl,--build-id=none -Wl,-Map=$work/build/libc-link.map"
for target_name in asicend asicen-ts asicenctl; do
    cmake --build "$work/build" --target "$target_name" --parallel 1
    python3 "$work/src/scripts/audit-linux-candidate.py" \
        --verify-link-map "$work/build/libc-link.map" \
        --library "$libc_archive" --base "$work/build"
    cp "$work/build/libc-link.map" "$work/$target_name.link.map"
done

nm_out=$(nm "$work/build/asicend")
for symbol in libusb_init libusb_wrap_sys_device libusb_close; do
    printf '%s\n' "$nm_out" | grep -E "[[:space:]]$symbol$" >/dev/null || {
        printf 'static libusb runtime symbol missing before stripping: %s\n' "$symbol" >&2; exit 1;
    }
done
for command_name in asicend asicen-ts asicenctl; do
    "$work/build/$command_name" --help >/dev/null
    cp "$work/build/$command_name" "$stage/bin/$command_name"
    strip --strip-all "$stage/bin/$command_name"
done
cat > "$stage/REBUILD.md" <<EOF
# Rebuild this private Linux command candidate

These binaries were built from this exact source archive (SHA-256 $source_sha).
The complete build inputs and runtime license texts are in the corresponding
source bundle. The commands below require a Linux musl GCC/G++ toolchain,
CMake, Ninja, GNU make, GNU tar/coreutils, gzip, pkgconf, and Linux headers.
They do not require firmware.

    mkdir project-src libusb-src libusb-prefix
    tar -xzf source/asicen-userland-source.tar.gz -C project-src
    python3 project-src/scripts/audit-linux-candidate.py --source-archive source/asicen-userland-source.tar.gz
    tar -xjf source/libusb-1.0.30.tar.bz2 -C libusb-src --strip-components=1
    cd libusb-src
    ./configure --prefix="\$PWD/../libusb-prefix" --disable-shared \\
      --enable-static --with-pic --disable-udev --disable-examples-build \\
      --disable-tests-build --disable-dependency-tracking
    make && make install
    cd ../project-src
    cmake -S . -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release \\
      -DASICEN_ENABLE_LIBUSB=ON -DASICEN_ENABLE_IFD=OFF -DASICEN_BUILD_TESTS=OFF \\
      -DASICEN_LIBUSB_INCLUDE_DIR="\$PWD/../libusb-prefix/include/libusb-1.0" \\
      -DASICEN_LIBUSB_LIBRARY="\$PWD/../libusb-prefix/lib/libusb-1.0.a" \\
      -DASICEN_LIBUSB_EXTRA_LIBRARIES=pthread \\
      -DCMAKE_CXX_FLAGS='-static -static-libstdc++ -static-libgcc' \\
      -DCMAKE_EXE_LINKER_FLAGS='-static -static-libstdc++ -static-libgcc'
    cmake --build ../build --target asicend asicen-ts asicenctl

For an LGPL relink, modify the extracted libusb source, rebuild the static
archive, and point CMake's \`ASICEN_LIBUSB_INCLUDE_DIR\` and
\`ASICEN_LIBUSB_LIBRARY\` at the modified headers/archive. Relink all three
commands as above. The pinned release builder checks the pristine source hash
for reproducibility; that check is not part of this manual relink procedure.

This intermediate command candidate deliberately contains no firmware and
no IFD plugin. Final Linux variants must be assembled with the matching IFD
build and externally supplied firmware. The firmware is excluded from the
corresponding source bundle. Verify a binary candidate with
\`sha256sum -c SHA256SUMS\`. For a standalone source bundle, compare its SHA-256
with the distributor's value, then run the source-archive audit above.
This is a private build input, not a release archive.
EOF
tar -tzf "$stage/source/asicen-userland-source.tar.gz" | LC_ALL=C sort > "$stage/SOURCE-MANIFEST.txt"
python3 "$work/src/scripts/audit-linux-candidate.py" --normalize-modes "$stage"
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -C "$stage" -cf "$work/corresponding-source.tar" \
    source/asicen-userland-source.tar.gz source/libusb-1.0.30.tar.bz2 \
    SOURCE-MANIFEST.txt REBUILD.md licenses
gzip -n -c "$work/corresponding-source.tar" > \
    "$stage/source/asicen-linux-corresponding-source.tar.gz"
chmod 0755 "$stage/bin/asicend" "$stage/bin/asicen-ts" "$stage/bin/asicenctl"
chmod 0644 "$stage/source/asicen-userland-source.tar.gz"

cat > "$stage/BUILD-METADATA.txt" <<EOF
Candidate class: intermediate Linux static command build; incomplete, not a release.
Architecture: $(uname -m)
Compiler target: $target
Compiler: $(g++ --version | head -n 1)
GCC executable SHA-256: $(sha256sum "$(realpath -e "$(command -v gcc)")" | awk '{print $1}')
G++ executable SHA-256: $(sha256sum "$(realpath -e "$(command -v g++)")" | awk '{print $1}')
Compiler sysroot: $(gcc -print-sysroot)
Musl libc archive: $libc_archive
Musl libc archive SHA-256: $libc_archive_sha
Musl libc loader: $libc_loader
Musl libc loader identification: $(printf '%s\n' "$loader_output" | grep -E -m 1 '^musl libc \(')
Musl libc loader SHA-256: $libc_loader_sha
Musl license source SHA-256: $musl_license_sha
GCC COPYING3 source SHA-256: $gcc_copying3_sha
GCC runtime exception source SHA-256: $gcc_runtime_exception_sha
Source archive SHA-256: $source_sha
libusb source SHA-256: $libusb_sha
EOF
python3 "$work/src/scripts/audit-linux-candidate.py" --normalize-modes "$stage"
(
    cd "$stage"
    sha256sum bin/asicend bin/asicen-ts bin/asicenctl \
        source/asicen-userland-source.tar.gz source/libusb-1.0.30.tar.bz2 \
        source/asicen-linux-corresponding-source.tar.gz \
        licenses/COPYING.gpl2 licenses/px4-userland-GPL-2.0-only.txt \
        licenses/libusb-COPYING licenses/NOTICES.md \
        licenses/musl-COPYRIGHT.txt licenses/GCC-COPYING3.txt \
        licenses/GCC-COPYING.RUNTIME.txt \
        SOURCE-MANIFEST.txt REBUILD.md BUILD-METADATA.txt > SHA256SUMS
)
python3 "$work/src/scripts/audit-linux-candidate.py" "$stage"
tar --sort=name --mtime='@0' --owner=0 --group=0 --numeric-owner \
    -C "$stage" -czf "$work/candidate.tar.gz" .

mv -- "$stage" "$output"
mv -- "$work/candidate.tar.gz" "$output.tar.gz"
rm -rf -- "$work"
trap - EXIT HUP INT TERM
printf 'private static candidate created: %s\n' "$output"
