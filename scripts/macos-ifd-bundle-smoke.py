#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compatibility entrypoint for the canonical IFD proxy smoke audit."""
from pathlib import Path
import runpy
import sys

if len(sys.argv) != 3:
    raise SystemExit("usage: macos-ifd-bundle-smoke.py IFD_LIBRARY MOCK_DAEMON")
sys.argv[1:] = ["--ifd-smoke-library", sys.argv[1], "--ifd-smoke-daemon", sys.argv[2]]
runpy.run_path(str(Path(__file__).with_name("audit-artifact.py")), run_name="__main__")
