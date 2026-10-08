#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]


def load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"could not import {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


package = load("asicen_macos_package", ROOT / "scripts/package-macos-candidate.py")
audit = load("asicen_macos_audit", ROOT / "scripts/audit-macos-candidate.py")


class MacDistributionTests(unittest.TestCase):
    def test_candidate_normalization_preserves_only_expected_executable_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary) / "stage"
            for relative in (
                "bin/asicend", "bin/asicen-ts", "bin/asicenctl",
                "pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib",
                "pcsc/ASICEN-IFD.bundle/Contents/Info.plist", "licenses/NOTICES.md",
            ):
                path = stage / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
                path.chmod(0o600)
            package.normalize_candidate_modes(stage)
            for relative in ("bin/asicend", "bin/asicen-ts", "bin/asicenctl",
                             "pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib"):
                self.assertEqual((stage / relative).stat().st_mode & 0o777, 0o755)
            for relative in ("pcsc/ASICEN-IFD.bundle/Contents/Info.plist",
                             "licenses/NOTICES.md"):
                self.assertEqual((stage / relative).stat().st_mode & 0o777, 0o644)

            archive = Path(temporary) / "candidate.tar.gz"
            package.write_candidate_archive(stage, archive)
            with tarfile.open(archive, "r:gz") as source:
                members = {member.name: member for member in source.getmembers()}
            self.assertEqual(members["bin/asicend"].mode & 0o777, 0o755)
            self.assertEqual(
                members["pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib"].mode & 0o777,
                0o755)
            self.assertEqual(members["licenses/NOTICES.md"].mode & 0o777, 0o644)

    def test_audit_accepts_only_exact_ifd_self_install_id(self):
        plugin = Path("/tmp/libifd-asicen.dylib")

        def output(*args):
            if args[:2] == ("lipo", "-archs"):
                return "arm64\n"
            if args[:2] == ("otool", "-D"):
                return f"{plugin}:\n@rpath/libifd-asicen.dylib\n"
            if args[:2] == ("otool", "-L"):
                return (f"{plugin}:\n@rpath/libifd-asicen.dylib (compatibility version 0.0.0)\n"
                        "/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n")
            if args[:2] == ("otool", "-l"):
                return "Load command 0\n"
            raise AssertionError(args)

        with mock.patch.object(audit, "command", side_effect=output):
            audit.audit_macho(plugin, "@rpath/libifd-asicen.dylib")

        def bad_id(*args):
            result = output(*args)
            if args[:2] == ("otool", "-D"):
                return f"{plugin}:\n@rpath/other.dylib\n"
            return result

        with mock.patch.object(audit, "command", side_effect=bad_id):
            with self.assertRaisesRegex(ValueError, "install ID"):
                audit.audit_macho(plugin, "@rpath/libifd-asicen.dylib")


if __name__ == "__main__":
    unittest.main()
