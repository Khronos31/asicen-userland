#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""macOS distribution regressions against the canonical artifact tools."""
import contextlib
import importlib.util
import io
from pathlib import Path
import plistlib
import sys
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


package = load("asicen_package", ROOT / "scripts/package-artifact.py")
audit = canonical_audit = load("asicen_audit", ROOT / "scripts/audit-artifact.py")


class MacDistributionTests(unittest.TestCase):
    def test_audit_cli_supports_directory_archive_and_combined_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            candidate = Path(temporary) / "candidate"
            candidate.mkdir()
            archive = Path(temporary) / "candidate.tar.gz"
            archive.write_bytes(b"fixture")
            for arguments, want_build, want_archive in (
                    (["--build-dir", str(candidate)], True, False),
                    (["--archive", str(archive)], False, True),
                    (["--archive", str(archive), "--build-dir", str(candidate)], True, True)):
                with mock.patch.object(sys, "argv", ["audit-artifact.py", "--platform", "darwin-arm64", *arguments]), \
                        mock.patch.object(audit, "audit_binaries", return_value={}) as audit_build, \
                        mock.patch.object(audit, "audit_binary_archive", return_value={}) as audit_archive, \
                        contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(audit.main(), 0)
                self.assertEqual(audit_build.call_count, int(want_build))
                self.assertEqual(audit_archive.call_count, int(want_archive))
            for arguments in ([], ["--source-archive"], ["--ifd-smoke-library", str(candidate)]):
                with mock.patch.object(sys, "argv", ["audit-artifact.py", *arguments]), \
                        contextlib.redirect_stderr(io.StringIO()):
                    with self.assertRaises(SystemExit) as error:
                        audit.main()
                self.assertEqual(error.exception.code, 2)

    def test_candidate_normalization_preserves_only_expected_executable_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary) / "stage"
            executable = audit.DARWIN_IFD_ARTIFACT
            plist = "ifd/asicen-userland-ifd.bundle/Contents/Info.plist"
            for relative in (*audit.PROGRAMS, executable, plist, "THIRD_PARTY_NOTICES.md"):
                path = stage / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture")
                path.chmod(0o600)
            package.normalize_stage_modes(stage)
            for relative in (*audit.PROGRAMS, executable):
                self.assertEqual((stage / relative).stat().st_mode & 0o777, 0o755)
            for relative in (plist, "THIRD_PARTY_NOTICES.md"):
                self.assertEqual((stage / relative).stat().st_mode & 0o777, 0o644)
            archive = Path(temporary) / "candidate.tar.gz"
            package.deterministic_tar(stage, archive)
            with tarfile.open(archive, "r:gz") as source:
                members = {member.name: member for member in source.getmembers()}
            self.assertEqual(members["asicend"].mode, 0o755)
            self.assertEqual(members[executable].mode, 0o755)
            self.assertEqual(members["THIRD_PARTY_NOTICES.md"].mode, 0o644)

    def test_audit_accepts_only_exact_ifd_self_install_id(self):
        plugin = Path("/tmp/libasicen-userland-ifd.dylib")
        helper = CanonicalMacDistributionTests()
        with mock.patch.object(audit, "run", side_effect=helper.command_output):
            audit.audit_darwin(plugin, audit.DARWIN_IFD_ARTIFACT)
        with mock.patch.object(audit, "run", side_effect=lambda args: helper.command_output(
                args, install_id="@rpath/other.dylib")):
            with self.assertRaisesRegex(audit.AuditError, "install ID"):
                audit.audit_darwin(plugin, audit.DARWIN_IFD_ARTIFACT)


class CanonicalMacDistributionTests(unittest.TestCase):
    def command_output(self, arguments, *, architecture="arm64", install_id=None, rpath=False):
        plugin = "/tmp/libasicen-userland-ifd.dylib"
        if arguments[:2] == ["lipo", "-archs"]:
            return architecture + "\n"
        if arguments[:2] == ["otool", "-D"]:
            return f"{plugin}:\n{install_id or '@rpath/libasicen-userland-ifd.dylib'}\n"
        if arguments[:2] == ["otool", "-L"]:
            return (f"{plugin}:\n@rpath/libasicen-userland-ifd.dylib (compatibility version 0.0.0)\n"
                    "/usr/lib/libSystem.B.dylib (compatibility version 1.0.0)\n")
        if arguments[:2] == ["otool", "-l"]:
            return "cmd LC_DYSYMTAB\nnlocalsym 0\n" + ("cmd LC_RPATH\n" if rpath else "")
        if arguments[1] == "-gU":
            return "\n".join("_" + name for name in canonical_audit.IFD_EXPORTS) + "\n"
        raise AssertionError(arguments)

    def test_exact_arm64_install_id_and_no_rpath_are_required(self):
        plugin = Path("/tmp/libasicen-userland-ifd.dylib")
        with mock.patch.object(canonical_audit, "run", side_effect=self.command_output):
            record = canonical_audit.audit_darwin(plugin, canonical_audit.DARWIN_IFD_ARTIFACT)
        self.assertEqual(record["install_id"], "@rpath/libasicen-userland-ifd.dylib")
        for options, diagnostic in (({"architecture": "x86_64"}, "arm64"),
                                    ({"architecture": "arm64 x86_64"}, "arm64"),
                                    ({"install_id": "@rpath/other.dylib"}, "install ID"),
                                    ({"rpath": True}, "LC_RPATH")):
            with self.subTest(options=options), mock.patch.object(
                    canonical_audit, "run", side_effect=lambda args: self.command_output(args, **options)):
                with self.assertRaisesRegex(canonical_audit.AuditError, diagnostic):
                    canonical_audit.audit_darwin(plugin, canonical_audit.DARWIN_IFD_ARTIFACT)

    def test_bundle_plist_preserves_asicen_identity_and_no_usb_registration(self):
        properties = {
            "CFBundleIdentifier": "io.github.khronos31.asicen-userland.ifd",
            "CFBundleExecutable": "libasicen-userland-ifd.dylib",
            "CFBundlePackageType": "BNDL",
        }
        canonical_audit.verify_darwin_bundle_plist(plistlib.dumps(properties))
        for key, value in (("CFBundleIdentifier", "foreign"), ("CFBundleExecutable", "wrong.dylib"),
                           ("CFBundlePackageType", "APPL"), ("ifdVendorID", "0b06"),
                           ("ifdProductID", "0005"), ("IOKitPersonalities", {})):
            with self.subTest(key=key), self.assertRaises(canonical_audit.AuditError):
                canonical_audit.verify_darwin_bundle_plist(plistlib.dumps({**properties, key: value}))


if __name__ == "__main__":
    unittest.main()
