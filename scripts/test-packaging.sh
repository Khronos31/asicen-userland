#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Adapted from px4-userland 1a1485d0c3e972e0a47be907edb67949564aa9a7.
# Local changes: ASICEN names, firmware policy, and retained runtime licenses.
set -eu
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(cd -- "$script_dir/.." && pwd)
shellcheck "$root/packaging/termux/asicen-termux" "$root/tests/test-asicen-termux.sh"
"$root/tests/test-asicen-termux.sh"
python3 "$script_dir/audit-artifact.py" --self-test
python3 "$script_dir/audit-artifact.py" --help | grep -F -- '--ndk-root' >/dev/null
python3 "$script_dir/package-artifact.py" --self-test
python3 "$script_dir/android-link-inventory.py" --help >/dev/null
"$script_dir/test-mdev.sh"
"$script_dir/test-workflow.sh"
"$script_dir/test-fedora.sh"
"$script_dir/test-report.sh"
test_root=$(mktemp -d /tmp/asicen-package-self-test.XXXXXX)
trap 'find "$test_root" -depth -delete' EXIT
version=$(tr -d '\n' < "$script_dir/../VERSION")

source_forbidden_case=0
for source_forbidden_member in \
    repository/scripts/__pycache__/generated.py \
    repository/scripts/generated.pyc \
    repository/scripts/generated.pyo \
    repository/scripts/generated.pyd; do
    source_forbidden_archive="$test_root/source-forbidden-$source_forbidden_case.tar.gz"
    python3 - "$source_forbidden_archive" "$source_forbidden_member" <<'PY'
import io
import sys
import tarfile

archive, name = sys.argv[1:]
with tarfile.open(archive, "w:gz") as stream:
    payload = b"forbidden\n"
    info = tarfile.TarInfo(name)
    info.size = len(payload)
    stream.addfile(info, io.BytesIO(payload))
PY
    if python3 "$script_dir/audit-artifact.py" --source-archive \
        --archive "$source_forbidden_archive" >/dev/null 2>&1; then
        printf '%s\n' "source archive accepted forbidden member: $source_forbidden_member" >&2
        exit 1
    fi
    source_forbidden_case=$((source_forbidden_case + 1))
done
printf '%s\n' 'source archive bytecode rejection tests: PASS'

mkdir -p "$test_root/build"
printf '%s\n' asicend asicen-ts asicenctl | while IFS= read -r program; do
    printf '%s\n' synthetic >"$test_root/build/$program"
    chmod 0755 "$test_root/build/$program"
done
mkdir -p "$test_root/ifd-build"
printf '%s\n' synthetic >"$test_root/ifd-build/ifd.so"
chmod 0755 "$test_root/ifd-build/ifd.so"
write_build_metadata() {
    python3 - "$test_root/build" "$1" "$test_root/ifd-build" "${2:-musl}" <<'PY'
import hashlib
import json
from pathlib import Path
import sys

root = Path(sys.argv[1])
metadata = {
    "schema": 1, "compiler_target": f"{sys.argv[2]}-linux-musl", "source_ref": "self-test",
    "libc_archive_sha256": "a" * 64, "libc_identification": "musl libc Version 1.2.5",
    "production_binary_sha256": {
        name: hashlib.sha256((root / name).read_bytes()).hexdigest()
        for name in ("asicend", "asicen-ts", "asicenctl")
    },
}
(root / "build-metadata.json").write_text(json.dumps(metadata) + "\n", encoding="utf-8")
ifd_root = Path(sys.argv[3])
ifd_metadata = {
    "schema": 1, "source_ref": "self-test",
    "compiler_target": f"{sys.argv[2]}-linux-{sys.argv[4]}",
    "library_sha256": hashlib.sha256((ifd_root / "ifd.so").read_bytes()).hexdigest(),
}
(ifd_root / "build-metadata.json").write_text(json.dumps(ifd_metadata) + "\n", encoding="utf-8")
PY
}
write_build_metadata x86_64
mkdir -p "$test_root/macos-build"
for program in asicend asicen-ts asicenctl; do
    # shellcheck disable=SC2016
    printf '%s\n' '#!/bin/sh' 'test "${1:-}" = --help' >"$test_root/macos-build/$program"
    chmod 0755 "$test_root/macos-build/$program"
done
mkdir -p "$test_root/ifd.bundle/Contents/MacOS"
sed 's/@ASICEN_IFD_BUNDLE_EXECUTABLE_NAME@/libasicen-userland-ifd.dylib/g; s/@PROJECT_VERSION@/0.1.0/g' \
    "$root/packaging/pcsc/macos/Info.plist.in" >"$test_root/ifd.bundle/Contents/Info.plist"
printf '%s\n' synthetic >"$test_root/ifd.bundle/Contents/MacOS/libasicen-userland-ifd.dylib"
PATH="$script_dir/testdata:$PATH" python3 "$script_dir/package-artifact.py" \
    --platform linux-musl-x86_64 --version "$version" --static-build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --source-ref self-test --output-dir "$test_root/out"
PATH="$script_dir/testdata:$PATH" python3 "$script_dir/package-artifact.py" \
    --platform linux-musl-x86_64 --version "$version" --static-build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --source-ref self-test --output-dir "$test_root/out-second"
first_sha=$(sha256sum "$test_root/out/asicen-userland-$version-linux-musl-x86_64.tar.gz" | awk '{print $1}')
second_sha=$(sha256sum "$test_root/out-second/asicen-userland-$version-linux-musl-x86_64.tar.gz" | awk '{print $1}')
[ "$first_sha" = "$second_sha" ] || {
    printf '%s\n' 'deterministic package self-test failed' >&2
    exit 1
}
if tar -xOzf "$test_root/out/asicen-userland-$version-linux-musl-x86_64.tar.gz" evidence/binary-audit.json | grep -F "$test_root" >/dev/null; then
    printf '%s\n' 'binary-audit.json leaked temporary input path' >&2
    exit 1
fi
tar -xOzf "$test_root/out/asicen-userland-$version-linux-musl-x86_64.tar.gz" \
    reader.conf.d/asicen-userland.conf | grep -F 'access=@ASICEN_ACCESS@' >/dev/null
for section_mode in debug zdebug symtab build-id-asicend; do
    if ASICEN_TEST_SECTIONS="$section_mode" PATH="$script_dir/testdata:$PATH" \
        python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 \
        --build-dir "$test_root/build" --ifd-library "$test_root/ifd-build/ifd.so"; then
        printf '%s\n' "negative Linux binary $section_mode audit test failed" >&2
        exit 1
    fi
    if ASICEN_TEST_SECTIONS="$section_mode" PATH="$script_dir/testdata:$PATH" \
        python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 \
        --archive "$test_root/out/asicen-userland-$version-linux-musl-x86_64.tar.gz"; then
        printf '%s\n' "negative Linux $section_mode audit test failed" >&2
        exit 1
    fi
done
ASICEN_TEST_SECTIONS=build-id-non-asicend PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 \
    --build-dir "$test_root/build" --ifd-library "$test_root/ifd-build/ifd.so"
ASICEN_TEST_SECTIONS=build-id-non-asicend PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 \
    --archive "$test_root/out/asicen-userland-$version-linux-musl-x86_64.tar.gz"
printf '%s\n' 'Linux IFD Build ID allowance tests: PASS'
ASICEN_TEST_SECTIONS=dynsym-unwind PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 \
    --archive "$test_root/out/asicen-userland-$version-linux-musl-x86_64.tar.gz"
ASICEN_TEST_STRIP_LOG="$test_root/strip.log" PATH="$script_dir/testdata:$PATH" python3 "$script_dir/package-artifact.py" \
    --platform darwin-arm64 --version "$version" --build-dir "$test_root/macos-build" \
    --ifd-bundle "$test_root/ifd.bundle" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --output-dir "$test_root/out-macos"
tar -xOzf "$test_root/out-macos/asicen-userland-$version-darwin-arm64.tar.gz" \
    evidence/binary-audit.json | grep -F '"dwarf_sections": []' >/dev/null
tar -xOzf "$test_root/out-macos/asicen-userland-$version-darwin-arm64.tar.gz" \
    evidence/binary-audit.json | grep -F '"nlocalsym": 0' >/dev/null
test "$(wc -l < "$test_root/strip.log")" -eq 4
grep -F -- '-S -x ' "$test_root/strip.log" >/dev/null
if grep -F "$test_root/build" "$test_root/strip.log" >/dev/null; then
    printf '%s\n' 'macOS strip touched the build tree' >&2
    exit 1
fi
ASICEN_TEST_OTOOL_MODE=allowed-local PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/package-artifact.py" \
    --platform darwin-arm64 --version "$version" --build-dir "$test_root/macos-build" \
    --ifd-bundle "$test_root/ifd.bundle" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --output-dir "$test_root/out-macos-allowed-local"
tar -xOzf "$test_root/out-macos-allowed-local/asicen-userland-$version-darwin-arm64.tar.gz" \
    evidence/binary-audit.json | grep -F '"nlocalsym": 1' >/dev/null
tar -xOzf "$test_root/out-macos-allowed-local/asicen-userland-$version-darwin-arm64.tar.gz" \
    evidence/binary-audit.json | grep -F 'radr://5614542' >/dev/null
for otool_mode in bad-dwarf bad-local bad-two bad-rpath; do
    if ASICEN_TEST_OTOOL_MODE="$otool_mode" PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/package-artifact.py" \
    --platform darwin-arm64 --version "$version" --build-dir "$test_root/macos-build" \
    --ifd-bundle "$test_root/ifd.bundle" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --output-dir "$test_root/out-macos-$otool_mode"; then
    printf '%s\n' "negative macOS $otool_mode audit test failed" >&2
    exit 1
    fi
done
if ASICEN_TEST_NM_MODE=bad-ifd-exports PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/package-artifact.py" \
    --platform darwin-arm64 --version "$version" --build-dir "$test_root/macos-build" \
    --ifd-bundle "$test_root/ifd.bundle" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --output-dir "$test_root/out-macos-bad-ifd-exports"; then
    printf '%s\n' 'negative macOS IFD export audit test failed' >&2
    exit 1
fi
python3_bin=$(command -v python3)
dirname_bin=$(command -v dirname)
no_otool_path="$test_root/no-otool-bin"
mkdir "$no_otool_path"
ln -s "$python3_bin" "$no_otool_path/python3"
ln -s "$dirname_bin" "$no_otool_path/dirname"
PATH="$no_otool_path" "$script_dir/audit-artifact.sh" \
    --platform darwin-arm64 \
    --archive "$test_root/out-macos/asicen-userland-$version-darwin-arm64.tar.gz"
PATH="$no_otool_path" "$script_dir/audit-artifact.sh" \
    --platform darwin-arm64 \
    --archive "$test_root/out-macos-allowed-local/asicen-userland-$version-darwin-arm64.tar.gz"
write_build_metadata aarch64
ASICEN_TEST_ARCH=aarch64 PATH="$script_dir/testdata:$PATH" python3 "$script_dir/package-artifact.py" \
    --platform linux-musl-aarch64 --version "$version" --static-build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --source-ref self-test --output-dir "$test_root/out-aarch64"
ASICEN_TEST_ARCH=aarch64 PATH="$script_dir/testdata:$PATH" python3 "$script_dir/audit-artifact.py" \
    --platform linux-musl-aarch64 \
    --archive "$test_root/out-aarch64/asicen-userland-$version-linux-musl-aarch64.tar.gz"
write_build_metadata aarch64 gnu
ASICEN_TEST_ARCH=aarch64 ASICEN_TEST_LIBC=glibc PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/package-artifact.py" \
    --platform linux-glibc-aarch64 --version "$version" --static-build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --source-ref self-test --output-dir "$test_root/out-glibc-aarch64"
ASICEN_TEST_ARCH=aarch64 ASICEN_TEST_LIBC=glibc PATH="$script_dir/testdata:$PATH" \
    python3 "$script_dir/audit-artifact.py" --platform linux-glibc-aarch64 \
    --archive "$test_root/out-glibc-aarch64/asicen-userland-$version-linux-glibc-aarch64.tar.gz"
write_build_metadata x86_64 gnu
ASICEN_TEST_LIBC=glibc PATH="$script_dir/testdata:$PATH" python3 "$script_dir/package-artifact.py" \
    --platform linux-glibc-x86_64 --version "$version" --static-build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so" \
    --reader-template "$script_dir/../packaging/pcsc/reader.conf.d/asicen-userland.conf.in" \
    --source-ref self-test --output-dir "$test_root/out-glibc"
ASICEN_TEST_LIBC=glibc PATH="$script_dir/testdata:$PATH" python3 "$script_dir/audit-artifact.py" \
    --platform linux-glibc-x86_64 \
    --archive "$test_root/out-glibc/asicen-userland-$version-linux-glibc-x86_64.tar.gz"
if PATH="$script_dir/testdata:$PATH" ASICEN_TEST_LINKAGE=bad-all \
    python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 --build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so"; then
    printf '%s\n' 'negative Linux libusb linkage test failed' >&2
    exit 1
fi
if PATH="$script_dir/testdata:$PATH" ASICEN_TEST_LIBC=glibc ASICEN_TEST_NEEDED=bad-glibc \
    python3 "$script_dir/audit-artifact.py" --platform linux-glibc-x86_64 --build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so"; then
    printf '%s\n' 'negative glibc IFD dependency test failed' >&2
    exit 1
fi
if PATH="$script_dir/testdata:$PATH" ASICEN_TEST_NEEDED=bad-musl \
    python3 "$script_dir/audit-artifact.py" --platform linux-musl-x86_64 --build-dir "$test_root/build" \
    --ifd-library "$test_root/ifd-build/ifd.so"; then
    printf '%s\n' 'negative musl IFD dependency test failed' >&2
    exit 1
fi
if PATH="$script_dir/testdata:$PATH" ASICEN_TEST_OTOOL_MODE=bad-pcsc \
    python3 "$script_dir/audit-artifact.py" --platform darwin-arm64 --build-dir "$test_root/macos-build" \
    --ifd-bundle "$test_root/ifd.bundle"; then
    printf '%s\n' 'negative macOS PCSC linkage test failed' >&2
    exit 1
fi
if PATH="$script_dir/testdata:$PATH" ASICEN_TEST_OTOOL_MODE=bad-ifd-libusb \
    python3 "$script_dir/audit-artifact.py" --platform darwin-arm64 --build-dir "$test_root/macos-build" \
    --ifd-bundle "$test_root/ifd.bundle"; then
    printf '%s\n' 'negative macOS IFD libusb linkage test failed' >&2
    exit 1
fi
for otool_mode in bad-libusb bad-homebrew; do
    if PATH="$script_dir/testdata:$PATH" ASICEN_TEST_OTOOL_MODE="$otool_mode" \
        python3 "$script_dir/audit-artifact.py" --platform darwin-arm64 --build-dir "$test_root/macos-build" \
        --ifd-bundle "$test_root/ifd.bundle"; then
        printf '%s\n' "negative macOS $otool_mode linkage test failed" >&2
        exit 1
    fi
done
tar -xOzf "$test_root/out-macos/asicen-userland-$version-darwin-arm64.tar.gz" \
    DEPENDENCY-NOTICE.txt | grep -Fx 'dependency.libusb.linkage=static' >/dev/null
tar -xOzf "$test_root/out-macos/asicen-userland-$version-darwin-arm64.tar.gz" \
    DEPENDENCY-NOTICE.txt | grep -Fx "corresponding-source-archive=asicen-userland-$version-source.tar.gz" >/dev/null
printf '%s\n' 'macOS static libusb linkage tests: PASS'
real_android_platform=${ASICEN_REAL_ANDROID_PLATFORM:-}
real_android_build_dir=${ASICEN_REAL_ANDROID_BUILD_DIR:-}
real_android_link_map_dir=${ASICEN_REAL_ANDROID_LINK_MAP_DIR:-}
real_android_ndk_root=${ASICEN_REAL_ANDROID_NDK_ROOT:-}
real_android_libusb_archive=${ASICEN_REAL_ANDROID_LIBUSB_SOURCE_ARCHIVE:-}
real_android_binary_suffix=${ASICEN_REAL_ANDROID_BINARY_SUFFIX:-}
if [ -n "$real_android_platform$real_android_build_dir$real_android_link_map_dir$real_android_ndk_root$real_android_libusb_archive" ]; then
    for required_input in \
        "$real_android_platform" "$real_android_build_dir" "$real_android_link_map_dir" \
        "$real_android_ndk_root" "$real_android_libusb_archive"; do
        [ -n "$required_input" ] || {
            printf '%s\n' 'incomplete ASICEN_REAL_ANDROID_* input set' >&2
            exit 1
        }
    done
    case "$real_android_platform" in
    android-aarch64|android-armv7a|android-x86_64) ;;
    *)
        printf '%s\n' "unsupported ASICEN_REAL_ANDROID_PLATFORM: $real_android_platform" >&2
        exit 1
        ;;
    esac
    real_android_version=$(python3 - "$script_dir/../VERSION" <<'PY'
from pathlib import Path
import sys

data = Path(sys.argv[1]).read_bytes()
if not data.endswith(b"\n") or data.count(b"\n") != 1:
    raise SystemExit("VERSION must contain exactly one newline")
print(data[:-1].decode("ascii"))
PY
)
    python3 "$script_dir/package-artifact.py" \
        --platform "$real_android_platform" --version "$real_android_version" \
        --build-dir "$real_android_build_dir" --binary-suffix "$real_android_binary_suffix" \
        --libusb-source-archive "$real_android_libusb_archive" \
        --ndk-root "$real_android_ndk_root" --link-map-dir "$real_android_link_map_dir" \
        --output-dir "$test_root/real-android"
    python3 "$script_dir/audit-artifact.py" \
        --platform "$real_android_platform" \
        --archive "$test_root/real-android/asicen-userland-$real_android_version-$real_android_platform.tar.gz"
    python3 - \
        "$test_root/real-android/asicen-userland-$real_android_version-$real_android_platform.tar.gz" \
        "$test_root" "$script_dir/.." "$real_android_build_dir" "$real_android_link_map_dir" "$real_android_ndk_root" <<'PY'
import pathlib
import sys
import tarfile

archive, temp_root, source_root, build_dir, link_map_dir, ndk_root = sys.argv[1:]
forbidden = tuple(str(pathlib.Path(path).resolve()) for path in
                  (temp_root, source_root, build_dir, link_map_dir, ndk_root))
with tarfile.open(archive, "r:gz") as stream:
    for member in stream.getmembers():
        if member.name.endswith(".map") or member.name.startswith("evidence/maps/"):
            raise SystemExit(f"raw linker map entered Android archive: {member.name}")
        handle = stream.extractfile(member)
        if handle is None:
            raise SystemExit(f"cannot read archive member: {member.name}")
        payload = handle.read()
        for path in forbidden:
            if path.encode() in payload:
                raise SystemExit(f"input path entered Android archive: {path}")
PY
    printf '%s\n' 'real Android package and final archive audit: PASS'
fi
real_libusb_archive=${ASICEN_LIBUSB_1_0_30_ARCHIVE:-}
if [ -n "$real_libusb_archive" ]; then
    [ -f "$real_libusb_archive" ] || {
        printf '%s\n' "ASICEN_LIBUSB_1_0_30_ARCHIVE is not a file: $real_libusb_archive" >&2
        exit 1
    }
    python3 "$script_dir/package-artifact.py" --self-test \
        --libusb-source-archive "$real_libusb_archive"
    source_ref=${ASICEN_SOURCE_REF:-HEAD}
    if git -C "$script_dir/.." cat-file -e "$source_ref:VERSION" 2>/dev/null; then
        python3 "$script_dir/package-source.py" --version "$version" \
            --source-root "$script_dir/.." --source-ref "$source_ref" \
            --libusb-source-archive "$real_libusb_archive" --output-dir "$test_root/source"
    else
        if python3 "$script_dir/package-source.py" --version "$version" \
            --source-root "$script_dir/.." --source-ref "$source_ref" \
            --libusb-source-archive "$real_libusb_archive" --output-dir "$test_root/source"; then
            printf '%s\n' 'source package accepted a source ref without VERSION' >&2
            exit 1
        fi
        printf '%s\n' 'source package correctly refused source ref without VERSION'
    fi
    source_archive="$test_root/source/asicen-userland-$version-source.tar.gz"
    if [ -f "$source_archive" ] && tar -tzf "$source_archive" | \
        grep -E '(^|/)(__pycache__/|.*\.py[co]$|.*\.pyd$)' >/dev/null; then
        printf '%s\n' 'source archive contains Python bytecode' >&2
        exit 1
    fi
fi
printf '%s\n' 'packaging self-tests: PASS'
