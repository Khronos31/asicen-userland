#!/usr/bin/env python3
"""Hardware-free asicend --hardware fd/bus:addr argument contract checks.

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
    result = run('--hardware', *args)
    check(result.returncode == 2,
          f'expected argument rejection for {args!r}, got {result.returncode}: '
          f'{result.stderr}')
    check('libusb_init' not in result.stderr,
          f'argument rejection for {args!r} must not reach libusb_init')


# bus:addr and fd selectors cannot be mixed in either order.
rejects('--primary', '1:2', '--primary-port', '1-2', '--primary-fd', '3')
rejects('--primary', '1:2', '--sibling', '1:3', '--primary-fd', '3',
        '--sibling-port', '1-4')
rejects('--primary-fd', '3', '--primary', '1:2', '--primary-port', '1-2')
rejects('--primary-fd', '3', '--sibling-port', '1-4', '--primary-port', '1-2')

# The same number cannot describe both functions.
rejects('--primary-fd', '5', '--sibling-fd', '5')

# A fd selector must be present, numeric, and non-negative.
rejects('--primary-fd')
rejects('--sibling-fd', '5', '--model', 's3u')
rejects('--primary-fd', 'not-a-number')
rejects('--primary-fd', '-1')
rejects('--primary-fd', '3', '--primary-fd', '4')

print('fd CLI checks passed (no hardware access)')
