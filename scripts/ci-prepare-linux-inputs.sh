#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Stage runner-local inputs for the Linux candidate jobs.
# Firmware and toolchain license texts are copied out of the checkout because
# the package and build scripts refuse inputs that still sit inside the tree.
# The corresponding-source snapshot continues to omit firmware/.
set -eu
umask 022

usage() {
    printf '%s\n' 'usage: ci-prepare-linux-inputs.sh --dest DIR'
}
dest=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --dest) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; dest=$2; shift 2 ;;
        --help) usage; exit 0 ;;
        *) printf 'unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
done
[ -n "$dest" ] || { usage >&2; exit 2; }

root=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd -P)
mkdir -p "$dest"
dest=$(CDPATH='' cd -- "$dest" && pwd -P)
case "$dest/" in "$root/"*) printf '%s\n' 'dest must be outside the checkout' >&2; exit 1 ;; esac

copy_checked() {
    src=$1
    out=$2
    expect=$3
    cp "$src" "$out"
    got=$(sha256sum "$out" | awk '{print $1}')
    [ "$got" = "$expect" ] || {
        printf 'unexpected SHA-256 for %s: %s\n' "$out" "$got" >&2
        exit 1
    }
}

copy_checked "$root/firmware/asicen-loader.bin" "$dest/asicen-loader.bin" \
    b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8
copy_checked "$root/distribution/licenses/musl-1.2.5-COPYRIGHT" "$dest/musl-1.2.5-COPYRIGHT" \
    f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af
copy_checked "$root/distribution/licenses/gcc-COPYING3" "$dest/gcc-COPYING3" \
    8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903
copy_checked "$root/distribution/licenses/gcc-COPYING.RUNTIME" "$dest/gcc-COPYING.RUNTIME" \
    9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74

libusb=$dest/libusb-1.0.30.tar.bz2
curl -fsSL --retry 2 \
    -o "$libusb" \
    https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30.tar.bz2
libusb_sha=$(sha256sum "$libusb" | awk '{print $1}')
[ "$libusb_sha" = fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf ] || {
    printf 'unexpected libusb SHA-256: %s\n' "$libusb_sha" >&2
    exit 1
}

sh "$root/scripts/make-linux-source-snapshot.sh" --output "$dest/asicen-userland-source.tar.gz"
printf '%s\n' "prepared Linux inputs in $dest"
