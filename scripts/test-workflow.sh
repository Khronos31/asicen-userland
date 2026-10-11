#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Offline guard for packaging workflow invariants.
set -eu
root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
workflow=$root/.github/workflows/build_userland.yml
test -f "$workflow"
if "$root/scripts/build-android.sh" --abi unsupported --output "/tmp/asicen-userland-invalid-abi-$$"; then
    printf '%s\n' 'build-android.sh accepted an unsupported ABI' >&2
    exit 1
fi
"$root/scripts/verify-android-elf.sh" --self-test >/dev/null
android_block=$(awk '
    $0 == "  android-api-24:" { in_job = 1; next }
    in_job && /^  [^ ]/ { exit }
    in_job { print }
' "$workflow")
printf '%s\n' "$android_block" | grep -F 'abi: x86_64' >/dev/null
printf '%s\n' "$android_block" | grep -F 'platform: android-x86_64' >/dev/null
printf '%s\n' "$android_block" | grep -F 'scripts/package-artifact.sh --platform' >/dev/null
printf '%s\n' "$android_block" | grep -F 'actions/upload-artifact@' >/dev/null
printf '%s\n' "$android_block" | grep -F 'shellcheck packaging/termux/asicen-termux' >/dev/null
grep -F 'android-api-24, source-archive' "$workflow" >/dev/null
grep -F 'android-x86_64' "$workflow" >/dev/null
grep -F 'len(actual) != 9' "$workflow" >/dev/null
grep -F 'android_x86_64_archive' "$workflow" >/dev/null
# Windows implementation, packaging and native tests are retained. Only the
# final release inclusion is commented out until the planned WinSCard release.
grep -F 'source-archive, windows-x86_64, windows-native-tests' "$workflow" >/dev/null
grep -F 'needs: windows-x86_64' "$workflow" >/dev/null
release_candidate_block=$(awk '
    $0 == "  release-candidate:" { in_job = 1; next }
    in_job && /^  [^ ]/ { exit }
    in_job { print }
' "$workflow")
release_candidate_active=$(printf '%s\n' "$release_candidate_block" | sed '/^[[:space:]]*#/d')
if printf '%s\n' "$release_candidate_active" | grep -E 'windows_archive|windows-x86_64.zip|win-candidate-tests|name: win-candidate' >/dev/null; then
    printf '%s\n' 'release-candidate must not contain Windows binaries or archives' >&2
    exit 1
fi
printf '%s\n' "$release_candidate_block" | grep -F '#     name: win-candidate' >/dev/null
printf '%s\n' "$release_candidate_block" | grep -F "# f'asicen-userland-{version}-windows-x86_64.zip'," >/dev/null
grep -F 'name: win-candidate-tests' "$workflow" >/dev/null
grep -F 'name: Upload Windows CI-only candidate archive (not release distribution)' "$workflow" >/dev/null
grep -F 'name: Extract and smoke the exact CI-only Windows archive' "$workflow" >/dev/null
grep -F 'name: Corresponding-source Windows static relink proof' "$workflow" >/dev/null
grep -F 'scripts/test-windows-static-relink.sh' "$workflow" >/dev/null
grep -F -- '--enable-static --disable-shared' "$root/scripts/build-windows.sh" >/dev/null
grep -F -- '--libusb-source-dir' "$root/scripts/build-windows.sh" >/dev/null
grep -F 'imports {WINDOWS_LIBUSB_DLL}; libusb is statically linked' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'https://libusb.info' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'expected two libusb version URLs before replacement' "$root/scripts/test-windows-static-relink.sh" >/dev/null
if grep -F -- '--enable-shared' "$root/scripts/build-windows.sh" >/dev/null; then
    printf '%s\n' 'Windows libusb build must not enable a shared library' >&2
    exit 1
fi
if grep -F 'missing Windows libusb DLL' "$root/scripts/audit-artifact.py" >/dev/null; then
    printf '%s\n' 'Windows audit must not require a shipped libusb DLL' >&2
    exit 1
fi
grep -F 'TERMUX_LAUNCHER' "$root/scripts/audit-artifact.py" >/dev/null
grep -F '"packaging" / "termux"' "$root/scripts/package-artifact.py" >/dev/null
# shellcheck disable=SC2016
test "$(grep -c 'docker run --rm -e GITHUB_SHA="\$GITHUB_SHA"' "$workflow")" -ge 4
# Every fresh container that builds the runner-owned checkout needs its own
# exact-path Git trust before querying source provenance. Source-archive
# relink containers have no checkout and use source-manifest.json instead.
awk '
    /docker run / {
        checkout = ($0 ~ /GITHUB_WORKSPACE:\/src/)
        if (checkout) checkout_count++
        trusted = 0
    }
    checkout && /^[[:space:]]*git config --global --add safe.directory \/src;$/ { trusted = 1 }
    checkout && /scripts\/build-linux-(static|ifd)\.sh/ && !trusted {
        print "checkout build lacks exact /src Git trust before line " NR > "/dev/stderr"
        exit 1
    }
    END {
        if (checkout_count != 5) {
            print "expected five checkout-mounted Linux build containers" > "/dev/stderr"
            exit 1
        }
    }
' "$workflow"
grep -F 'linux-glibc-x86_64' "$workflow" >/dev/null
grep -F 'linux-musl-x86_64' "$workflow" >/dev/null
grep -F 'linux-glibc-aarch64' "$workflow" >/dev/null
grep -F 'linux-musl-aarch64' "$workflow" >/dev/null
test "$(grep -c 'scripts/test-static-relink.sh --libusb-source-archive /src/third_party/libusb-1.0.30.tar.bz2' "$workflow")" -eq 2
test "$(grep -c 'name: Corresponding-source static relink proof' "$workflow")" -eq 3
grep -F 'source-relink-x86_64, source-relink-aarch64, source-relink-macos' "$workflow" >/dev/null
macos_block=$(awk '
    $0 == "  macos:" { in_job = 1; next }
    in_job && /^  [^ ]/ { exit }
    in_job { print }
' "$workflow")
printf '%s\n' "$macos_block" | grep -F 'scripts/build-macos-static.sh --build-dir build-userland' >/dev/null
if printf '%s\n' "$macos_block" | grep -E 'brew install.*[[:space:]]libusb([[:space:]]|$)' >/dev/null; then
    printf '%s\n' 'macOS release build must not use Homebrew libusb' >&2
    exit 1
fi
# shellcheck disable=SC2016
grep -F 'scripts/test-static-relink.sh --libusb-source-archive "$source_root/third_party/libusb-1.0.30.tar.bz2"' "$workflow" >/dev/null
# shellcheck disable=SC2016
grep -F 'ASICEN_LIBUSB_LIBRARY="$prefix/lib/libusb-1.0.a"' "$root/scripts/build-macos-static.sh" >/dev/null
# The macOS libusb build must keep the Android reproducibility contract:
# optimized PIC, no debug flag at any spelling, and prefix maps that hide the
# randomized mktemp work tree.  Run the same helper the build uses.
. "$root/scripts/libusb-reproducibility.sh"
libusb_cflags=$(libusb_reproducible_cflags \
    /tmp/asicen-macos-static.cflags-check/libusb /tmp/asicen-macos-static.cflags-check)
case " $libusb_cflags " in
*" -O2 "*) ;;
*) printf '%s\n' 'macOS libusb CFLAGS must be optimized' >&2; exit 1 ;;
esac
case " $libusb_cflags " in
*" -fPIC "*) ;;
*) printf '%s\n' 'macOS libusb CFLAGS must build position-independent code' >&2; exit 1 ;;
esac
case " $libusb_cflags " in
*" -fdebug-compilation-dir=. "*) ;;
*) printf '%s\n' 'macOS libusb CFLAGS must pin the compilation directory' >&2; exit 1 ;;
esac
# shellcheck disable=SC2086
for debug_flag in $libusb_cflags; do
    case "$debug_flag" in
    -g*)
        printf '%s\n' "macOS libusb CFLAGS must not contain a debug flag: $debug_flag" >&2
        exit 1
        ;;
    esac
done
for mapped in /tmp/asicen-macos-static.cflags-check/libusb /tmp/asicen-macos-static.cflags-check; do
    for map_flag in -ffile-prefix-map -fdebug-prefix-map -fmacro-prefix-map; do
        case " $libusb_cflags " in
        *" $map_flag=$mapped=. "*) ;;
        *)
            printf '%s\n' "macOS libusb CFLAGS missing $map_flag=$mapped=." >&2
            exit 1
            ;;
        esac
    done
done
# The build must source the same helper and pass the flags to its configure step.
# shellcheck disable=SC2016
grep -F '. "$root/scripts/libusb-reproducibility.sh"' "$root/scripts/build-macos-static.sh" >/dev/null
# shellcheck disable=SC2016
grep -F 'libusb_cflags=$(libusb_reproducible_cflags "$work/libusb" "$work")' "$root/scripts/build-macos-static.sh" >/dev/null
# shellcheck disable=SC2016
grep -F 'CFLAGS="$libusb_cflags" ./configure' "$root/scripts/build-macos-static.sh" >/dev/null
test "$(grep -c 'packaged-musl-' "$workflow")" -eq 2
test "$(grep -c 'ASICEN_BUILD_TESTS=ON' "$workflow")" -ge 3
test "$(grep -c 'ctest --test-dir' "$workflow")" -ge 3
if grep -E -- 'package-artifact\.sh --platform linux-(x86_64|aarch64)( |$)' "$workflow" >/dev/null; then
    printf '%s\n' 'workflow contains an ambiguous generic Linux package' >&2
    exit 1
fi
test "$(grep -c 'sudo chown -R' "$workflow")" -ge 2
test "$(grep -c 'apt_install_retry()' "$workflow")" -eq 2
# shellcheck disable=SC2016
test "$(grep -c '\"$attempt\" -le 3' "$workflow")" -eq 2
test "$(grep -c 'find /var/lib/apt/lists -mindepth 1 -depth -delete' "$workflow")" -eq 2
test "$(grep -c 'debian:11@sha256:' "$workflow")" -eq 2
digest_values=$(grep -o 'debian:11@sha256:[0-9a-f]*' "$workflow" | sort -u)
test "$(printf '%s\n' "$digest_values" | grep -c .)" -eq 1
digest=${digest_values#debian:11@sha256:}
case "$digest" in
    *[!0-9a-f]*|'')
        printf '%s\n' 'Debian image digest must be lowercase hexadecimal' >&2
        exit 1
        ;;
esac
test "${#digest}" -eq 64
test "$(grep -c 'http://snapshot.debian.org/archive/debian/20260825T000000Z bullseye main' "$workflow")" -eq 2
test "$(grep -c 'http://snapshot.debian.org/archive/debian-security/20260825T000000Z bullseye-security main' "$workflow")" -eq 2
test "$(grep -c 'check-valid-until=no' "$workflow")" -eq 4
if grep -E 'debian:11([[:space:]]|$)' "$workflow" >/dev/null; then
    printf '%s\n' 'Debian 11 image must be digest-pinned' >&2
    exit 1
fi
if grep -F 'find /var/lib/apt/lists -mindepth 1 -maxdepth 1' "$workflow" >/dev/null; then
    printf '%s\n' 'apt list cleanup must be recursive' >&2
    exit 1
fi
grep -F 'expected two libusb version URLs before replacement' "$root/scripts/test-static-relink.sh" >/dev/null
# shellcheck disable=SC2016
grep -F '[ "$modified_marker_count" -ge 1 ]' "$root/scripts/test-static-relink.sh" >/dev/null
# shellcheck disable=SC2016
grep -F '"$strip_tool" --strip-all' "$root/scripts/build-linux-static.sh" >/dev/null
# shellcheck disable=SC2016
grep -F '"$strip_tool" --strip-unneeded' "$root/scripts/build-linux-ifd.sh" >/dev/null
grep -F 'def audit_elf_sections' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'reject_build_id: bool' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'GNU Build ID section is forbidden in release ELF' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'ASICEN_RELEASE_ASICEND_NO_BUILD_ID' "$root/CMakeLists.txt" >/dev/null
grep -F 'if(ASICEN_RELEASE_ASICEND_NO_BUILD_ID AND TARGET asicend)' "$root/CMakeLists.txt" >/dev/null
grep -F 'target_link_options(asicend PRIVATE "LINKER:--build-id=none")' "$root/CMakeLists.txt" >/dev/null
# shellcheck disable=SC2016
grep -F -- '-DASICEN_RELEASE_ASICEND_NO_BUILD_ID=ON' "$root/scripts/build-linux-static.sh" >/dev/null
# shellcheck disable=SC2016
grep -F -- '-DASICEN_RELEASE_ASICEND_NO_BUILD_ID=ON' "$root/scripts/build-android.sh" >/dev/null
grep -F -- 'LINKER:--build-id=none' "$root/CMakeLists.txt" >/dev/null
grep -F 'segname\s+__DWARF' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'nlocalsym' "$root/scripts/audit-artifact.py" >/dev/null
if grep -F '"-N"' "$root/scripts/package-artifact.py" >/dev/null; then
    printf '%s\n' 'Darwin packaging must not use strip -N' >&2
    exit 1
fi
grep -F '"-S", "-x"' "$root/scripts/package-artifact.py" >/dev/null
grep -F 'IFD_EXPORTS' "$root/scripts/audit-artifact.py" >/dev/null
grep -F 'strip_darwin_stage' "$root/scripts/package-artifact.py" >/dev/null
grep -F 'command -v strip' "$workflow" >/dev/null
grep -F 'apt-get install --no-install-recommends -y binutils cmake ninja-build' "$workflow" >/dev/null
grep -F 'Install native ELF audit tools' "$workflow" >/dev/null
# shellcheck disable=SC2016
grep -F 'Authorization: Bearer $GITHUB_TOKEN' "$root/.github/workflows/check-libusb.yml" >/dev/null
# ASICEN keeps hardware identity and release validation policy in its own docs.
# Check the common distribution contract without importing PX4 receiver/serial prose.
grep -F 'access=@ASICEN_ACCESS@' "$root/packaging/pcsc/reader.conf.d/asicen-userland.conf.in" >/dev/null
grep -F 'instance=@ASICEN_INSTANCE@' "$root/packaging/pcsc/reader.conf.d/asicen-userland.conf.in" >/dev/null
grep -F 'macOS' "$root/SPEC.md" >/dev/null
grep -F 'Windows' "$root/SPEC.md" >/dev/null
grep -F 'musl' "$root/SPEC.md" >/dev/null
test "$(grep -c 'name: Release candidate Linux x86_64 Ubuntu artifact smoke' "$workflow")" -eq 1
test "$(grep -c 'name: Release candidate Linux x86_64 Alpine artifact smoke' "$workflow")" -eq 1
test "$(grep -c 'name: Release candidate Linux aarch64 Ubuntu artifact smoke' "$workflow")" -eq 1
test "$(grep -c 'name: Release candidate Linux aarch64 Alpine artifact smoke' "$workflow")" -eq 1
test "$(grep -c 'name: Run exact candidate static CLIs and glibc IFD on Ubuntu$' "$workflow")" -eq 1
test "$(grep -c 'name: Run exact candidate static CLIs and glibc IFD on Ubuntu arm64' "$workflow")" -eq 1
test "$(grep -c 'name: Run exact candidate static CLIs and musl IFD in Alpine$' "$workflow")" -eq 1
test "$(grep -c 'name: Run exact candidate static CLIs and musl IFD in Alpine arm64' "$workflow")" -eq 1
test "$(grep -c 'needs: release-candidate' "$workflow")" -eq 4
for job in \
    release-candidate-linux-ubuntu-x86_64 \
    release-candidate-linux-alpine-x86_64 \
    release-candidate-linux-ubuntu-aarch64 \
    release-candidate-linux-alpine-aarch64; do
    block=$(awk -v job="$job" '
        $0 == "  " job ":" { in_job = 1; next }
        in_job && /^  [^ ]/ { exit }
        in_job { print }
    ' "$workflow")
    checkout_line=$(printf '%s\n' "$block" | grep -n 'actions/checkout@' | head -n 1 | cut -d: -f1)
    download_line=$(printf '%s\n' "$block" | grep -n 'actions/download-artifact@' | head -n 1 | cut -d: -f1)
    test -n "$checkout_line" && test -n "$download_line" && test "$checkout_line" -lt "$download_line"
done
printf '%s\n' 'workflow packaging invariants: PASS'
