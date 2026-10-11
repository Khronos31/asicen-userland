#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Deterministic CFLAGS for the pinned libusb that release builds compile outside
# CMake.  The Android build defines this contract; the macOS build and its
# offline regression guard source the same helper so the flags cannot drift.

# Print the libusb CFLAGS.  Each argument is a directory that is prefix-mapped
# to "." so the randomized work tree never enters recorded file, debug, or macro
# paths.  -g is intentionally absent so the autoconf default cannot reintroduce
# debug info that varies with the temporary directory.
libusb_reproducible_cflags() {
    cflags="-O2 -fPIC -fdebug-compilation-dir=."
    for prefix_map in "$@"; do
        cflags="$cflags -ffile-prefix-map=$prefix_map=."
        cflags="$cflags -fdebug-prefix-map=$prefix_map=."
        cflags="$cflags -fmacro-prefix-map=$prefix_map=."
    done
    printf '%s\n' "$cflags"
}
