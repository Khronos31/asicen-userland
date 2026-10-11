#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("asicen_linux_audit", ROOT / "scripts/audit-artifact.py")
assert SPEC is not None and SPEC.loader is not None
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


class LinuxIfdAllowlistTests(unittest.TestCase):
    def test_glibc_allowlist_matches_verified_arm64_dependencies(self):
        self.assertEqual(audit.LINUX_TARGETS["linux-glibc-aarch64"]["needed"],
                         {"libc.so.6", "libpthread.so.0", "ld-linux-aarch64.so.1"})
        CanonicalLinuxIfdAllowlistTests().check("linux-glibc-aarch64", {"libc.so.6", "libpthread.so.0"})

    def test_x86_glibc_allowlist_retains_loader_allowance(self):
        self.assertEqual(audit.LINUX_TARGETS["linux-glibc-x86_64"]["needed"],
                         {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"})

    def test_verified_musl_allowlists_are_architecture_specific(self):
        self.assertEqual(audit.LINUX_TARGETS["linux-musl-aarch64"]["libc"], "libc.musl-aarch64.so.1")
        self.assertEqual(audit.LINUX_TARGETS["linux-musl-x86_64"]["libc"], "libc.musl-x86_64.so.1")

    def test_forbidden_dynamic_dependencies_still_fail(self):
        for architecture in ("aarch64", "x86_64"):
            for forbidden in ("libusb-1.0.so.0", "libstdc++.so.6"):
                with self.subTest(architecture=architecture, forbidden=forbidden):
                    with self.assertRaisesRegex(audit.AuditError, "unexpected host ABI dependency"):
                        CanonicalLinuxIfdAllowlistTests().check(
                            f"linux-glibc-{architecture}", {"libc.so.6", "libpthread.so.0", forbidden})

    def test_unknown_machine_is_rejected(self):
        with self.assertRaisesRegex(audit.AuditError, "unsupported platform"):
            audit.validate_platform("linux-glibc-unknown")


class CanonicalLinuxIfdAllowlistTests(unittest.TestCase):
    def check(self, platform, needed, *, exports=None, machine=None, glibc=None, shared=True):
        target = audit.LINUX_TARGETS[platform]

        def output(arguments):
            if arguments[0] == "nm":
                return "\n".join(audit.IFD_EXPORTS if exports is None else exports)
            option = arguments[1]
            if option == "-h":
                return "ELF Header:\n  Machine: " + (machine or target["machine"]) + "\n"
            if option == "-d":
                return "\n".join(f"Shared library: [{name}]" for name in needed)
            if option == "--version-info" and glibc:
                return f"Name: GLIBC_{glibc}\n"
            return ""

        with mock.patch.object(audit, "readelf_path", return_value="readelf"), \
                mock.patch.object(audit, "nm_path", return_value="nm"), \
                mock.patch.object(audit, "run", side_effect=output):
            return audit.audit_linux(Path("ifd.so"), "ifd/asicen-userland-ifd.so", platform=platform,
                                     shared=shared, require_libusb=False, reject_build_id=False,
                                     reject_pcsc=True)

    def test_glibc_subset_contract_allows_native_loader_but_requires_libc(self):
        for architecture, loader in (("x86_64", "ld-linux-x86-64.so.2"),
                                     ("aarch64", "ld-linux-aarch64.so.1")):
            platform = f"linux-glibc-{architecture}"
            self.check(platform, {"libc.so.6"})
            self.check(platform, {"libc.so.6", "libpthread.so.0", loader}, glibc="2.31")
            for forbidden in ("libusb-1.0.so.0", "libstdc++.so.6", "libpcsclite.so.1", "libdl.so.2"):
                with self.subTest(platform=platform, forbidden=forbidden), self.assertRaises(audit.AuditError):
                    self.check(platform, {"libc.so.6", forbidden})
            with self.assertRaises(audit.AuditError):
                self.check(platform, {"libpthread.so.0"})

    def test_musl_contract_requires_exact_matching_libc(self):
        for architecture in ("x86_64", "aarch64"):
            platform = f"linux-musl-{architecture}"
            libc = f"libc.musl-{architecture}.so.1"
            self.check(platform, {libc})
            for needed in (set(), {"libc.so.6"}, {libc, "libpthread.so.0"}):
                with self.subTest(platform=platform, needed=needed), self.assertRaises(audit.AuditError):
                    self.check(platform, needed)

    def test_architecture_glibc_floor_and_exact_exports_remain_guarded(self):
        for options in ({"machine": "AArch64"}, {"glibc": "2.32"},
                        {"exports": audit.IFD_EXPORTS[:-1]},
                        {"exports": (*audit.IFD_EXPORTS, "extra_export")}):
            with self.subTest(options=options), self.assertRaises(audit.AuditError):
                self.check("linux-glibc-x86_64", {"libc.so.6"}, **options)
        with self.assertRaisesRegex(audit.AuditError, "GLIBC version"):
            self.check("linux-musl-x86_64", set(), glibc="2.2.5", shared=False)


if __name__ == "__main__":
    unittest.main()
