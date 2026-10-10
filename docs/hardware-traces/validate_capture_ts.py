#!/usr/bin/env python3
"""Compatibility entry for the shared, hardware-independent TS report tool.

Exit zero means a report was generated; inspect its counters for capture quality.
Historical report fields and the synthetic self-test remain unchanged.
"""
from pathlib import Path
import runpy


if __name__ == "__main__":
    runpy.run_path(
        str(Path(__file__).resolve().parents[2] / "userland" / "tools" / "ts_validation.py"),
        run_name="__main__",
    )
