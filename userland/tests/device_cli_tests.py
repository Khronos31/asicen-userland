#!/usr/bin/env python3
"""Hardware-free asicend --device/--firmware argument contract checks.

Every case here fails during argument parsing or firmware verification,
before the enclosure lock, libusb_init, or any USB access.
"""
import pathlib
import re
import subprocess
import sys
import tempfile

build = pathlib.Path(sys.argv[1])


def run(*args):
    return subprocess.run([str(build / 'asicend'), *args], capture_output=True,
                          text=True, timeout=5)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def rejects(expected, *args):
    result = run(*args)
    check(result.returncode == expected,
          f'expected exit {expected} for {args!r}, got {result.returncode}: '
          f'{result.stderr}')
    check('libusb_init' not in result.stderr and 'enclosure identity' not in result.stderr,
          f'rejection for {args!r} must not reach USB access')


# Removed selectors and model selection have no alias.
rejects(2, '--usb-path', '1-2', '--instance', 'preflight')
rejects(2, '--device', 'usb:1-2', '--instance', 'preflight', '--model', 'w3u3')
# Core syntax errors, position pairs outside one enclosure, and combinations.
rejects(2, '--device', 'usb:1:2', '--instance', 'preflight')
rejects(2, '-d', 'usb:1-2.1,1-2.1', '--instance', 'preflight')
rejects(2, '-d', 'usb:1-2.1,1-3.2', '--instance', 'preflight')
rejects(2, '-d', 'usb:1-2.1,1-2.3', '--instance', 'preflight')
rejects(2, '--device', 'usb:1-2', '--fd', '3', '--instance', 'preflight')
rejects(2, '--device', 'usb:1-2')
# Valid selectors no ASICEN function can satisfy.
rejects(3, '--device', 'serial:ABC123', '--instance', 'preflight')
rejects(3, '--device', 'usb:1-256', '--instance', 'preflight')
rejects(3, '-d', 'usb:1-2.1,1-2.2,1-2.3', '--instance', 'preflight')

with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / 'invalid-product-firmware.bin'
    path.write_bytes(bytes(16384))
    for option in ('--firmware', '-F'):
        for candidate, expected in ((path, 10), (path.with_name('missing.bin'), 3)):
            rejects(expected, '--device', 'usb:1-1', '--instance', 'preflight',
                    option, str(candidate))
            rejects(expected, '-d', 'usb:1-2.2,1-2.1', '--instance', 'preflight',
                    option, str(candidate))

help_text = run('--help')
check(help_text.returncode == 0 and '--device usb:BUS-PORT' in help_text.stdout and
      '--usb-path' not in help_text.stdout and
      re.search(r'--model(?!s)', help_text.stdout) is None,
      help_text.stdout)

print('device CLI checks passed (no hardware access)')
