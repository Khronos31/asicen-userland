#!/usr/bin/env python3
"""Hardware-free asicend usb-path/fd argument contract checks.

Every case here fails during argument parsing, before the enclosure lock,
libusb_init, or any USB access.
"""
import pathlib
import subprocess
import sys

build = pathlib.Path(sys.argv[1])


def run(*args):
    return subprocess.run([str(build / 'asicend'), *args], capture_output=True,
                          text=True, timeout=5)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def rejects(*args):
    result = run(*args)
    check(result.returncode == 2,
          f'expected argument rejection for {args!r}, got {result.returncode}: '
          f'{result.stderr}')
    check('libusb_init' not in result.stderr,
          f'argument rejection for {args!r} must not reach libusb_init')


# --usb-path and --fd cannot be mixed.
rejects('--usb-path', '1:2', '--fd', '3')
rejects('--fd', '3', '--usb-path', '1:2', '--usb-path', '1:3')

# A selector is required; exactly one selector family.
rejects('--model', 'w3u3')
rejects('--fd')

# --usb-path must be a valid BUS:ADDRESS or BUS-PORT value at most two distinct.
rejects('--usb-path', 'not-a-location')
rejects('--usb-path', '1:2', '--usb-path', '1:2')
rejects('--usb-path', '1-2-3')
rejects('--usb-path', '1:256')

# --fd values are numeric, distinct, and at most two.
rejects('--fd', 'not-a-number')
rejects('--fd', '-1')
rejects('--fd', '5', '--fd', '5')

print('fd CLI checks passed (no hardware access)')