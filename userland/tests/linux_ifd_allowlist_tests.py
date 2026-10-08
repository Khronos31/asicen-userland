#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "asicen_linux_package", ROOT / "scripts/package-linux-variant.py")
assert SPEC is not None and SPEC.loader is not None
package = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(package)


class LinuxIfdAllowlistTests(unittest.TestCase):
    def test_glibc_allowlist_matches_verified_arm64_dependencies(self):
        self.assertEqual(
            package.allowed_ifd_needed("glibc", "AArch64"),
            {"libc.so.6", "libpthread.so.0"})

    def test_x86_glibc_allowlist_retains_loader_requirement(self):
        self.assertEqual(
            package.allowed_ifd_needed("glibc", "Advanced Micro Devices X86-64"),
            {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"})

    def test_verified_musl_allowlists_are_architecture_specific(self):
        self.assertEqual(package.allowed_ifd_needed("musl", "AArch64"),
                         {"libc.musl-aarch64.so.1"})
        self.assertEqual(package.allowed_ifd_needed(
            "musl", "Advanced Micro Devices X86-64"),
            {"libc.musl-x86_64.so.1"})

    def test_forbidden_dynamic_dependencies_still_fail(self):
        for machine, allowed in (
            ("AArch64", {"libc.so.6", "libpthread.so.0"}),
            ("Advanced Micro Devices X86-64",
             {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"}),
        ):
            for forbidden in ("libusb-1.0.so.0", "libstdc++.so.6"):
                with self.subTest(machine=machine, forbidden=forbidden):
                    with self.assertRaisesRegex(ValueError, "allowlist mismatch"):
                        package.validate_ifd_needed(allowed | {forbidden}, "glibc", machine)

    def test_unknown_machine_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "unsupported glibc IFD machine"):
            package.allowed_ifd_needed("glibc", "unknown")


if __name__ == "__main__":
    unittest.main()
