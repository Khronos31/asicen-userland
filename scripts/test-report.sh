#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Exercise the report collector with synthetic sysfs and tools; never touch USB.
set -eu
script_dir=$(cd -- "$(dirname -- "$0")" && pwd)
python3 - "$script_dir/asicen-report.sh" <<'PYTEST'
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

report = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix="asicen-report-tests-") as temporary:
    root = Path(temporary)
    bindir = root / "bin"
    sysfs = root / "sysfs"
    work = root / "work"
    for directory in (bindir, sysfs, work):
        directory.mkdir()
    for topology in ("1-2.1", "1-2.2"):
        device = sysfs / topology
        device.mkdir()
        for name, value in (("idVendor", "0b06"), ("idProduct", "0005"), ("speed", "480")):
            (device / name).write_text(value + "\n")
    trace = root / "trace.jsonl"
    firmware = root / "firmware.bin"
    firmware.write_bytes(b"synthetic fixture only")
    tool = r"""#!/usr/bin/env python3
import json
import os
from pathlib import Path
import signal
import sys
import time
name = Path(sys.argv[0]).name
with open(os.environ["REPORT_TEST_TRACE"], "a", encoding="utf-8") as stream:
    stream.write(json.dumps([name] + sys.argv[1:]) + "\n")
if name == "asicend":
    if "--help" in sys.argv:
        raise SystemExit(0)
    if os.environ.get("REPORT_TEST_FAIL_DAEMON") == "1":
        print("synthetic daemon failure", flush=True)
        raise SystemExit(1)
    signal.signal(signal.SIGINT, lambda *_: sys.exit(0))
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    print("asicend ready", flush=True)
    while True:
        time.sleep(0.01)
if name == "dmesg":
    print("usb 1-2.1 idVendor=0b06:0005 synthetic fixture")
"""
    for name in ("asicend", "asicenctl", "asicen-ts", "lsusb", "dmesg"):
        path = bindir / name
        path.write_text(tool)
        path.chmod(0o755)
    base = dict(os.environ)
    for name in list(base):
        if name.startswith("ASICEN_") or name.startswith("REPORT_TEST_"):
            del base[name]
    base.update(PATH=str(bindir) + os.pathsep + base["PATH"],
                TMPDIR=str(work), ASICEN_BINDIR=str(bindir),
                ASICEN_REPORT_SYSFS=str(sysfs), REPORT_TEST_TRACE=str(trace))

    def run(extra, expected=0):
        trace.write_text("")
        result = subprocess.run([str(report)], env=dict(base, **extra),
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, timeout=15, check=False)
        assert result.returncode == expected, (result.returncode, result.stdout, result.stderr)
        assert not list(work.iterdir()), "private report/runtime directory leaked"
        calls = [json.loads(line) for line in trace.read_text().splitlines()]
        return result.stdout, calls

    output, calls = run({})
    assert "explicit topology/model selection" in output
    assert not any(call[0] == "asicend" and "--usb-path" in call for call in calls)
    selection = dict(ASICEN_REPORT_USB_PATH="1-2.1", ASICEN_REPORT_MODEL="w3u3")
    run(selection, 2)
    paired = dict(selection, ASICEN_REPORT_USB_PATH2="1-2.2", ASICEN_FIRMWARE=str(firmware))
    output, calls = run(paired)
    daemon = next(call for call in calls if call[0] == "asicend" and "--usb-path" in call)
    assert daemon.count("--usb-path") == 2
    assert daemon[daemon.index("--firmware") + 1] == str(firmware)
    assert daemon[daemon.index("--instance") + 1] == "report"
    runtime = Path(daemon[daemon.index("--runtime-dir") + 1])
    assert runtime.parent == work and not runtime.exists()
    assert "ready=1" in output
    controls = [call[-1] for call in calls if call[0] == "asicenctl"]
    assert controls == ["list", "status", "card-status", "card-atr", "status"]
    captures = [call for call in calls if call[0] == "asicen-ts"]
    assert len(captures) == 2
    assert [call[call.index("--receiver") + 1] for call in captures] == ["1", "0"]
    assert captures[1][captures[1].index("--lnb-voltage") + 1] == "0"
    output, calls = run(dict(paired, ASICEN_REPORT_TUNE="0"))
    assert not any(call[0] == "asicen-ts" for call in calls)
    output, calls = run(dict(ASICEN_REPORT_USB_PATH="1-2.1", ASICEN_REPORT_MODEL="s3u"))
    captures = [call for call in calls if call[0] == "asicen-ts"]
    assert [call[call.index("--receiver") + 1] for call in captures] == ["0", "0"]
    assert all("--firmware" not in call for call in calls if call[0] == "asicend")
    output, calls = run(dict(paired, REPORT_TEST_FAIL_DAEMON="1"))
    assert "ready=0" in output and "synthetic daemon failure" in output
    assert not any(call[0] == "asicenctl" for call in calls)
    run(dict(selection, ASICEN_REPORT_MODEL="unknown"), 2)
    empty = root / "empty-sysfs"
    empty.mkdir()
    run(dict(ASICEN_REPORT_SYSFS=str(empty)), 1)
    long_root = root / ("long-" + "x" * 100)
    long_root.mkdir()
    output, calls = run(dict(paired, TMPDIR=str(long_root)))
    daemon = next(call for call in calls if call[0] == "asicend" and "--usb-path" in call)
    runtime = Path(daemon[daemon.index("--runtime-dir") + 1])
    assert len(os.fsencode(runtime)) <= 64 and not runtime.exists()
    assert not list(long_root.iterdir()), "long TMPDIR report directory leaked"
print("report collector offline tests: PASS (9 cases)")
PYTEST
