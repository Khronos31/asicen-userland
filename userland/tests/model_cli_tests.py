#!/usr/bin/env python3
"""Hardware-free model/firmware CLI contract checks. Never submit a valid upload."""
import pathlib
import subprocess
import sys
import tempfile

build = pathlib.Path(sys.argv[1])
def run(binary, *args):
    return subprocess.run([str(build / binary), *args], capture_output=True,
                          text=True, timeout=5)

def check(condition, message):
    if not condition:
        raise AssertionError(message)

catalog = run('asicend', '--models')
check(catalog.returncode == 0, catalog.stderr)
rows = catalog.stdout.splitlines()
check(len(rows) == 5, catalog.stdout)
check(rows == ['PX-S3U 0b06:0001', 'PX-S3U2 0b06:0003', 'PX-W3U2 0b06:0004',
               'PX-W3U3 0b06:0005', 'PX-W3U3 V2 0b06:0006'], catalog.stdout)
check(not catalog.stderr, catalog.stderr)

version = (pathlib.Path(__file__).resolve().parents[2] / 'VERSION').read_text().strip()
for binary, label in [('asicend', 'asicend'), ('asicen-ts', 'asicen-ts'),
                      ('asicenctl', 'asicen-userland')]:
    result = run(binary, '--version')
    check(result.returncode == 0 and result.stdout == f'{label} {version}\n' and
          not result.stderr, (binary, result.returncode, result.stdout, result.stderr))
    for other in [('--help',), ('--version',), ('--instance', 'version-test')]:
        for arguments in [('--version', *other), (*other, '--version')]:
            result = run(binary, *arguments)
            check(result.returncode == 2 and not result.stdout,
                  (binary, arguments, result.returncode, result.stdout, result.stderr))

help_long = run('asicend', '--help')
help_short = run('asicend', '-h')
check(help_long.returncode == help_short.returncode == 0 and
      help_long.stdout == help_short.stdout and not help_short.stderr, help_short.stderr)
for arguments in [('-h', '--models'), ('--models', '-h'), ('-h', '--version'),
                  ('--instance', 'help-test', '-h'), ('-l',)]:
    result = run('asicend', *arguments)
    check(result.returncode == 2 and not result.stdout, (arguments, result.stderr))
check(run('asicend-mock', '--model', 'not-an-asicen').returncode == 2,
      'unknown mock model must fail before a server is created')
check(run('asicend', '--model', 'not-an-asicen').returncode in (2, 3),
      'unknown hardware model must fail before device access')
if (build / 'asicen-probe').exists():
    with tempfile.TemporaryDirectory() as directory:
        path = pathlib.Path(directory) / 'not-vendor-firmware.bin'
        path.write_bytes(bytes(16384))
        missing_model = run('asicen-probe', '--device', '1:1', '--firmware', str(path), 'load-firmware')
        check(missing_model.returncode == 2, missing_model.stderr)
        for model in ('s3u', 's3u2', 'w3u2', 'w3u3', 'w3u3-v2'):
            result = run('asicen-probe', '--device', '1:1', '--model', model,
                         '--firmware', str(path), 'load-firmware')
            check(result.returncode == 10 and 'firmware load failed: FIRMWARE_REJECTED' in result.stderr,
                  result.stderr)
            check('libusb_init' not in result.stderr and 'open 1:1' not in result.stderr,
                  'rejected firmware must not reach USB access')
print('model CLI checks passed (no hardware access)')
