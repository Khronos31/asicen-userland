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
for key, pid, count in [('s3u', '0001', 1), ('s3u2', '0003', 2),
                        ('w3u2', '0004', 4), ('w3u3', '0005', 4),
                        ('w3u3-v2', '0006', 4)]:
    row = next((r for r in rows if r.startswith(key + ' ')), '')
    check(f'vid_pid=0b06:{pid}' in row and f'capacity={count}' in row, row)
    check('family=' in row and 'runtime=' in row, row)
    if key in ('s3u', 's3u2'):
        check('lnb_control=external-unconfirmed' in row and
              'lnb_15v_request=unsupported' in row, row)
    else:
        check('lnb_control=source-backed-software' in row and
              'lnb_15v_request=supported' in row, row)
    check('lnb_validation=pending' in row, row)
check('verified' not in catalog.stdout, 'catalog must not imply hardware validation')
check(run('asicend', '--mock', '--model', 'not-an-asicen').returncode == 2,
      'unknown mock model must fail before a server is created')
check(run('asicend', '--hardware', '--model', 'not-an-asicen').returncode in (2, 3),
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
            check(result.returncode == 1 and 'firmware rejected' in result.stderr,
                  result.stderr)
            check('libusb_init' not in result.stderr and 'open 1:1' not in result.stderr,
                  'rejected firmware must not reach USB access')
print('model CLI checks passed (no hardware access)')
