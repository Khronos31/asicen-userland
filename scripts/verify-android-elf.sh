#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Verify an Android executable satisfies the hardened Bionic ELF contract.
set -eu

verify_load_alignments()
{
    alignments=$1
    [ -n "$alignments" ] || { printf '%s\n' 'no PT_LOAD segments' >&2; return 1; }
    for alignment in $alignments; do
        value=$((alignment))
        [ "$value" -ge 16384 ] || {
            printf '%s\n' "PT_LOAD alignment is below 16 KiB: $alignment" >&2
            return 1
        }
        [ $((value & (value - 1))) -eq 0 ] || {
            printf '%s\n' "PT_LOAD alignment is not a power of two: $alignment" >&2
            return 1
        }
    done
}

verify_machine()
{
    expected_arch=$1
    machine=$2
    case "$expected_arch:$machine" in
    aarch64:AArch64|aarch64:AARCH64|aarch64:ARM\ aarch64) ;;
    armv7a:ARM|armv7a:Arm) ;;
    x86_64:X86-64|x86_64:x86-64|x86_64:Advanced\ Micro\ Devices\ X86-64) ;;
    *) return 1 ;;
    esac
}

if [ "$#" -eq 1 ] && [ "$1" = '--self-test' ]; then
    if verify_load_alignments '0x1000'; then
        printf '%s\n' 'self-test failed: 0x1000 was accepted' >&2
        exit 1
    fi
    verify_load_alignments '0x4000'
    if verify_load_alignments '0x6000'; then
        printf '%s\n' 'self-test failed: non-power-of-two alignment was accepted' >&2
        exit 1
    fi
    verify_machine x86_64 'Advanced Micro Devices X86-64'
    if verify_machine x86_64 AArch64; then
        printf '%s\n' 'self-test failed: x86_64 accepted AArch64 machine' >&2
        exit 1
    fi
    if verify_machine aarch64 'Advanced Micro Devices X86-64'; then
        printf '%s\n' 'self-test failed: aarch64 accepted x86_64 machine' >&2
        exit 1
    fi
    printf '%s\n' 'alignment and architecture self-test: invalid alignments rejected, x86_64 distinguished from AArch64'
    exit 0
fi

[ "$#" -eq 3 ] || {
    printf '%s\n' "usage: $0 BINARY /system/bin/linker[64] aarch64|armv7a|x86_64" >&2
    exit 2
}
binary=$1
expected_interpreter=$2
expected_arch=$3
[ -f "$binary" ] || { printf '%s\n' "missing ELF: $binary" >&2; exit 1; }
case "$expected_interpreter" in
/system/bin/linker|/system/bin/linker64) ;;
*) printf '%s\n' "invalid expected interpreter: $expected_interpreter" >&2; exit 2 ;;
esac
case "$expected_arch" in
aarch64|armv7a|x86_64) ;;
*) printf '%s\n' "invalid expected architecture: $expected_arch" >&2; exit 2 ;;
esac

if command -v readelf >/dev/null 2>&1; then
    readelf_bin=$(command -v readelf)
elif command -v llvm-readelf >/dev/null 2>&1; then
    readelf_bin=$(command -v llvm-readelf)
else
    printf '%s\n' 'readelf is required' >&2
    exit 1
fi
header=$($readelf_bin -h "$binary")
program=$($readelf_bin -lW "$binary")
dynamic=$($readelf_bin -d "$binary")

machine=$(printf '%s\n' "$header" | awk -F: '/Machine:/ {gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
verify_machine "$expected_arch" "$machine" || {
    printf '%s\n' "unexpected machine for $expected_arch: $machine" >&2
    exit 1
}
type=$(printf '%s\n' "$header" | awk -F: '/Type:/ {gsub(/^[[:space:]]+/, "", $2); print $2; exit}')
case "$type" in
DYN*|*'shared object'*) ;;
*) printf '%s\n' "expected PIE DYN ELF, got $type" >&2; exit 1 ;;
esac
interpreter=$(printf '%s\n' "$program" | sed -n 's/.*Requesting program interpreter: \([^]]*\)].*/\1/p' | head -n 1)
[ "$interpreter" = "$expected_interpreter" ] || {
    printf '%s\n' "unexpected interpreter: $interpreter" >&2
    exit 1
}

if printf '%s\n' "$($readelf_bin -SW "$binary")" |
    awk '$2 ~ /^\.debug_/ || $2 == ".symtab" || $2 == ".strtab" {found=1} END {exit !found}'; then
    printf '%s\n' 'unstripped debug or regular symbol table section is forbidden' >&2
    exit 1
fi

load_alignments=$(printf '%s\n' "$program" | awk '$1 == "LOAD" {print $NF}')
verify_load_alignments "$load_alignments"

if ! printf '%s\n' "$program" | grep -E 'GNU_RELRO' >/dev/null; then
    printf '%s\n' 'missing GNU_RELRO segment' >&2
    exit 1
fi
if ! printf '%s\n' "$dynamic" |
    grep -E '\(FLAGS\).*BIND_NOW|\(FLAGS_1\).*NOW' >/dev/null; then
    printf '%s\n' 'missing BIND_NOW dynamic flag' >&2
    exit 1
fi

if printf '%s\n' "$dynamic" | grep -E '\(RPATH\)|\(RUNPATH\)|Library rpath|Library runpath' >/dev/null; then
    printf '%s\n' 'RPATH/RUNPATH is forbidden' >&2
    exit 1
fi
needed=$(printf '%s\n' "$dynamic" | sed -n 's/.*Shared library: \[\([^]]*\)\].*/\1/p')
for library in $needed; do
    case "$library" in
    libc.so|libm.so|libdl.so|liblog.so) ;;
    *) printf '%s\n' "unexpected DT_NEEDED: $library" >&2; exit 1 ;;
    esac
done
if ! printf '%s\n' "$needed" | grep -Fx 'libc.so' >/dev/null; then
    printf '%s\n' 'missing DT_NEEDED libc.so' >&2
    exit 1
fi
if printf '%s\n' "$needed" | grep -E 'libusb|libudev|libc\.so\.6|ld-linux|ld-musl|libc\.musl' >/dev/null; then
    printf '%s\n' 'host/libusb/glibc/musl dependency leaked into DT_NEEDED' >&2
    exit 1
fi
if printf '%s\n' "$program$dynamic" | grep -E '/lib64/ld-linux|/lib/ld-musl|/lib/ld-linux|/usr/|/opt/|data/data/com\.termux' >/dev/null; then
    printf '%s\n' 'host loader or prefix path leaked into ELF' >&2
    exit 1
fi
printf '%s\n' "verified $binary: $machine, $interpreter, RELRO/BIND_NOW, allowed DT_NEEDED, no RPATH/RUNPATH, PT_LOAD alignment >= 16 KiB (power-of-two)"
