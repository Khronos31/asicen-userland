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

models = run('asicend', '--models')
check(models.returncode == 0, models.stderr)
expected = [
    'PX-S3U 1738:5211 0b06:0001',
    'PX-S3U2 1738:5211 0b06:0003',
    'PX-W3U2 1738:5211 0b06:0004',
    'PX-W3U3 1738:5211 0b06:0005',
    'PX-W3U3 V2 1738:5211 0b06:0006',
]
check(models.stdout.splitlines() == expected, models.stdout)
check(not models.stdout.startswith('{'),
      '--models is not the JSON discovery of connected devices')
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
