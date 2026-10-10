#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Compatibility entry point; accepts the canonical package-artifact.py arguments."""
from pathlib import Path
import runpy

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).with_name("package-artifact.py")), run_name="__main__")
