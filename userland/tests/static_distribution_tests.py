#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Distribution safety regressions for the canonical packaging contract."""
import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts/audit-artifact.py"


def load_canonical(name):
    path = SCRIPT.parent / f"{name}.py"
    module_spec = importlib.util.spec_from_file_location(name.replace("-", "_"), path)
    assert module_spec is not None and module_spec.loader is not None
    module = importlib.util.module_from_spec(module_spec)
    module_spec.loader.exec_module(module)
    return module


audit = canonical_audit = load_canonical("audit-artifact")
package = canonical_package = load_canonical("package-artifact")
canonical_source = load_canonical("package-source")


class StaticDistributionAuditTests(unittest.TestCase):
    def test_source_archive_rejects_vendor_firmware(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                payload = b"not actual firmware"
                info = tarfile.TarInfo("repository/firmware/asicen-loader.bin")
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))
            with self.assertRaisesRegex(audit.AuditError, "firmware"):
                audit.audit_source_archive(argparse.Namespace(archive=path))

    def test_source_archive_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                info = tarfile.TarInfo("../outside")
                info.size = 1
                archive.addfile(info, io.BytesIO(b"x"))
            with self.assertRaisesRegex(audit.AuditError, "unsafe archive member"):
                audit.audit_source_archive(argparse.Namespace(archive=path))

    def test_static_glibc_fixture_is_not_certified_as_musl(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "fixture.cpp"
            executable = root / "glibc-static"
            source.write_text("int main() { return 0; }\n", encoding="utf-8")
            result = subprocess.run(
                ["g++", "-static", "-Wl,--build-id=none", str(source), "-o", str(executable)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            if result.returncode != 0:
                self.skipTest("host has no static glibc development archive")
            self.assertIn("Machine:", audit.run(["readelf", "-h", str(executable)]))
            self.assertNotIn("INTERP", audit.run(["readelf", "-l", str(executable)]))
            self.assertNotIn("NEEDED", audit.run(["readelf", "-d", str(executable)]))
            hashes = {name: package.sha256(executable) for name in audit.PROGRAMS}
            metadata = {"schema": 1, "compiler_target": "x86_64-linux-gnu",
                        "libc_archive_sha256": "a" * 64, "libc_identification": "glibc static archive",
                        "production_binary_sha256": hashes}
            with self.assertRaisesRegex(audit.AuditError, "musl compiler target"):
                audit.verify_linux_build_metadata(json.dumps(metadata).encode(), hashes, "linux-musl-x86_64")

    def test_candidate_root_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            real = root / "real"
            real.mkdir()
            link = root / "candidate"
            link.symlink_to(real, target_is_directory=True)
            with self.assertRaisesRegex(audit.AuditError, "real directory"):
                audit.audit_binaries(argparse.Namespace(platform="linux-musl-x86_64", build_dir=link))

    def test_corresponding_source_archive_rejects_directory_entries(self):
        # Canonical archives deliberately require regular files only.
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                info = tarfile.TarInfo("repository")
                info.type = tarfile.DIRTYPE
                archive.addfile(info)
            with self.assertRaisesRegex(audit.AuditError, "not a regular file"):
                audit.audit_source_archive(argparse.Namespace(archive=path))

    def test_link_map_normalizes_relative_compiler_archive_path(self):
        builder = (ROOT / "scripts/build-linux-static.sh").read_text()
        validator = builder.split("<<'PYMAP'\n", 1)[1].split("\nPYMAP", 1)[0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / "build"
            base.mkdir()
            libc = root / "toolchain/lib/libc.a"
            libc.parent.mkdir(parents=True)
            libc.write_bytes(b"libc archive")
            link_map = root / "link.map"
            link_map.write_text("LOAD ../toolchain/lib/./libc.a\n")
            command = [sys.executable, "-", str(link_map), str(libc), str(base)]
            result = subprocess.run(command, input=validator, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            wrong_libc = libc.with_name("wrong-libc.a")
            wrong_libc.write_bytes(b"wrong archive")
            link_map.write_text("LOAD ../other/lib/libc.a\n")
            result = subprocess.run(command, input=validator, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)

    def test_candidate_modes_and_archive_are_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "candidate"
            root.mkdir()
            (root / "asicend").write_bytes(b"command")
            (root / "notice").write_bytes(b"notice")
            (root / "notice").chmod(0o600)
            package.normalize_stage_modes(root)
            self.assertEqual(root.stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / "asicend").stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / "notice").stat().st_mode & 0o777, 0o644)
            first, second = Path(temporary) / "first.tar.gz", Path(temporary) / "second.tar.gz"
            package.deterministic_tar(root, first)
            package.deterministic_tar(root, second)
            self.assertEqual(package.sha256(first), package.sha256(second))

    def test_final_inventory_is_variant_specific_and_closed(self):
        glibc = audit.expected_binary_members("linux-glibc-x86_64")
        musl = audit.expected_binary_members("linux-musl-x86_64")
        self.assertEqual(glibc, musl)  # ABI identity is carried by platform and metadata.
        self.assertIn("firmware/asicen-loader.bin", glibc)
        self.assertIn("ifd/asicen-userland-ifd.so", musl)
        self.assertIn("toolchain/musl-1.2.5-COPYRIGHT", glibc)
        self.assertIn(audit.LINUX_IFD_METADATA_MEMBER, musl)
        self.assertNotIn("firmware/extra.bin", musl)
        self.assertNotIn("libusb-1.0.dll", audit.expected_binary_members("windows-x86_64"))

    def test_retained_build_metadata_must_match_source_snapshot(self):
        metadata = {"schema": 1, "source_ref": "a" * 40,
                    "compiler_target": "x86_64-linux-gnu", "library_sha256": "c" * 64}
        payload = json.dumps(metadata).encode()
        audit.verify_linux_ifd_metadata(payload, "c" * 64, "linux-glibc-x86_64", "a" * 40)
        with self.assertRaisesRegex(audit.AuditError, "same source_ref"):
            audit.verify_linux_ifd_metadata(payload, "c" * 64, "linux-glibc-x86_64", "b" * 40)

    def test_ifd_rejects_wrong_libc_and_source_snapshot(self):
        metadata = {"schema": 1, "source_ref": "a" * 40,
                    "compiler_target": "x86_64-linux-musl", "library_sha256": "c" * 64}
        payload = json.dumps(metadata).encode()
        with self.assertRaisesRegex(audit.AuditError, "architecture/libc"):
            audit.verify_linux_ifd_metadata(payload, "c" * 64, "linux-glibc-x86_64", "a" * 40)
        with self.assertRaisesRegex(audit.AuditError, "same source_ref"):
            audit.verify_linux_ifd_metadata(payload, "c" * 64, "linux-musl-x86_64", "b" * 40)

    def test_ifd_rejects_wrong_architecture_and_missing_exports(self):
        def output(arguments):
            if arguments[1] == "-h":
                return "ELF Header:\nMachine: Advanced Micro Devices X86-64\n"
            if arguments[1] == "-d":
                return "Shared library: [libc.musl-x86_64.so.1]\n"
            return ""
        with mock.patch.object(audit, "readelf_path", return_value="readelf"), \
                mock.patch.object(audit, "nm_path", return_value="nm"), \
                mock.patch.object(audit, "run", side_effect=output):
            with self.assertRaisesRegex(audit.AuditError, "ELF machine"):
                audit.audit_linux(Path("ifd.so"), "ifd/asicen-userland-ifd.so", platform="linux-musl-aarch64",
                                  shared=True, require_libusb=False, reject_build_id=False)
            with self.assertRaisesRegex(audit.AuditError, "export set"):
                audit.audit_linux(Path("ifd.so"), "ifd/asicen-userland-ifd.so", platform="linux-musl-x86_64",
                                  shared=True, require_libusb=False, reject_build_id=False)

    def test_firmware_inputs_and_final_inventory_are_guarded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            firmware = root / audit.FIRMWARE_MEMBER
            firmware.parent.mkdir()
            firmware.write_bytes(b"x")
            with self.assertRaisesRegex(package.AuditError, "vendor component"):
                package.copy_firmware(root, root / "stage")
            firmware.unlink()
            firmware.symlink_to(ROOT / audit.FIRMWARE_MEMBER)
            with self.assertRaisesRegex(package.AuditError, "firmware input"):
                package.copy_firmware(root, root / "stage")
            expected = audit.expected_binary_members("linux-musl-x86_64")
            self.assertTrue({audit.FIRMWARE_MEMBER, audit.FIRMWARE_NOTICE_MEMBER,
                             *audit.LINUX_RUNTIME_LICENSE_SHA256}.issubset(expected))
            output = root / "out"
            output.mkdir()
            version = (ROOT / "VERSION").read_text().strip()
            existing = output / f"asicen-userland-{version}-linux-musl-x86_64.tar.gz"
            existing.write_text("existing archive\n")
            arguments = ["package-artifact.py", "--platform", "linux-musl-x86_64", "--version", version,
                         "--static-build-dir", str(root), "--output-dir", str(output)]
            with mock.patch.object(sys, "argv", arguments):
                with self.assertRaisesRegex(package.AuditError, "refusing to overwrite"):
                    package.main()
            self.assertEqual(existing.read_text(), "existing archive\n")


class CanonicalStaticDistributionTests(unittest.TestCase):
    """Additional canonical archive, firmware, and source regressions."""

    def test_canonical_source_rejects_firmware_and_unsafe_members(self):
        for name in ("repository/firmware/asicen-loader.bin", "../outside", "repository/a/../b"):
            with self.subTest(name=name), tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "source.tar.gz"
                with tarfile.open(path, "w:gz") as archive:
                    entry = tarfile.TarInfo(name)
                    entry.size = 1
                    archive.addfile(entry, io.BytesIO(b"x"))
                with self.assertRaises(canonical_audit.AuditError):
                    canonical_audit.audit_source_archive(argparse.Namespace(archive=path))

    def test_archive_rejects_duplicate_alias_directory_and_symlink_entries(self):
        for kind in ("duplicate", "alias", "directory", "symlink"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temporary:
                path = Path(temporary) / "bad.tar.gz"
                with tarfile.open(path, "w:gz") as archive:
                    entry = tarfile.TarInfo("repository//file" if kind == "alias" else "repository/file")
                    if kind == "directory":
                        entry.type = tarfile.DIRTYPE
                    elif kind == "symlink":
                        entry.type = tarfile.SYMTYPE
                        entry.linkname = "other"
                    else:
                        entry.size = 1
                    archive.addfile(entry, io.BytesIO(b"x"))
                    if kind == "duplicate":
                        archive.addfile(entry, io.BytesIO(b"x"))
                with self.assertRaises(canonical_audit.AuditError):
                    canonical_audit.archive_members(path, forbidden=None)

    def test_fixture_option_dispatch_ignores_random_path_fragments(self):
        fixture = SCRIPT.parent / "testdata/readelf"
        output = subprocess.check_output([str(fixture), "-d", "/tmp/asicen-audit-h123/ifd.so"], text=True)
        self.assertIn("Shared library:", output)
        self.assertNotIn("ELF Header:", output)

    def test_exact_git_source_snapshot_uses_commit_and_export_attributes(self):
        root = SCRIPT.parent.parent
        commit = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
        tree = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD^{tree}"], text=True).strip()
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            self.assertEqual(canonical_source.add_git_snapshot(root, commit, stage), (commit, tree))
            self.assertFalse((stage / "repository/firmware").exists())
            self.assertEqual((stage / "repository/VERSION").read_bytes(), subprocess.check_output(
                ["git", "-C", str(root), "show", f"{commit}:VERSION"]))
            # These source inputs live in *.d directories, which the compiler
            # dependency ignore pattern also matches. A working-tree-only
            # packaging test must not hide an omitted tracked template.
            for name in (
                "packaging/pcsc/reader.conf.d/asicen-userland.conf.in",
                "packaging/fedora/sysusers.d/asicen-userland.conf",
            ):
                self.assertEqual((stage / "repository" / name).read_bytes(), subprocess.check_output(
                    ["git", "-C", str(root), "show", f"{commit}:{name}"]))

    def test_tracked_firmware_copy_retains_exact_notice_and_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            stage = Path(temporary)
            canonical_package.copy_firmware(SCRIPT.parent.parent, stage)
            payload = (stage / canonical_audit.FIRMWARE_MEMBER).read_bytes()
            self.assertEqual(len(payload), 16384)
            self.assertEqual(hashlib.sha256(payload).hexdigest(), canonical_audit.FIRMWARE_SHA256)
            self.assertEqual((stage / canonical_audit.FIRMWARE_NOTICE_MEMBER).read_text(), canonical_audit.FIRMWARE_NOTICE)
            for name in (canonical_audit.FIRMWARE_MEMBER, canonical_audit.FIRMWARE_NOTICE_MEMBER):
                self.assertEqual((stage / name).stat().st_mode & 0o777, 0o644)

    def test_firmware_rejects_wrong_bytes_missing_input_and_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            with self.assertRaisesRegex(canonical_package.AuditError, "firmware input"):
                canonical_package.copy_firmware(root, stage)
            source = root / canonical_audit.FIRMWARE_MEMBER
            source.parent.mkdir()
            source.write_bytes(b"x" * 16384)
            with self.assertRaisesRegex(canonical_package.AuditError, "vendor component"):
                canonical_package.copy_firmware(root, stage)
            source.unlink()
            source.symlink_to(SCRIPT.parent.parent / canonical_audit.FIRMWARE_MEMBER)
            with self.assertRaisesRegex(canonical_package.AuditError, "firmware input"):
                canonical_package.copy_firmware(root, stage)

    def test_firmware_notice_and_modes_cannot_be_changed(self):
        payload = (SCRIPT.parent.parent / canonical_audit.FIRMWARE_MEMBER).read_bytes()
        modes = {canonical_audit.FIRMWARE_MEMBER: 0o644, canonical_audit.FIRMWARE_NOTICE_MEMBER: 0o644}
        with self.assertRaisesRegex(canonical_audit.AuditError, "unresolved rights"):
            canonical_audit.verify_firmware(payload, b"rights cleared\n", modes)
        modes[canonical_audit.FIRMWARE_MEMBER] = 0o755
        with self.assertRaisesRegex(canonical_audit.AuditError, "mode 644"):
            canonical_audit.verify_firmware(payload, canonical_audit.FIRMWARE_NOTICE.encode(), modes)

    def test_linux_runtime_licenses_are_exact_and_tamper_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            canonical_package.copy_linux_runtime_licenses(SCRIPT.parent.parent, root / "stage")
            for name, expected in canonical_audit.LINUX_RUNTIME_LICENSE_SHA256.items():
                self.assertEqual(canonical_package.sha256(root / "stage" / name), expected)
            source = root / "distribution/licenses/gcc-COPYING3"
            source.parent.mkdir(parents=True)
            source.write_text("tampered\n")
            with self.assertRaisesRegex(canonical_package.AuditError, "pinned text"):
                canonical_package.copy_linux_runtime_licenses(root, root / "bad")

    def test_static_metadata_rejects_glibc_wrong_version_and_binary_mismatch(self):
        hashes = {name: hashlib.sha256(name.encode()).hexdigest() for name in canonical_audit.PROGRAMS}
        metadata = {
            "schema": 1, "compiler_target": "x86_64-linux-musl", "libc_archive_sha256": "a" * 64,
            "libc_identification": "musl libc Version 1.2.5", "production_binary_sha256": hashes,
        }
        canonical_audit.verify_linux_build_metadata(json.dumps(metadata).encode(), hashes, "linux-musl-x86_64")
        for field, value in (("compiler_target", "x86_64-linux-gnu"),
                             ("compiler_target", "aarch64-linux-musl"),
                             ("libc_identification", "glibc static archive"),
                             ("libc_identification", "musl libc Version 1.2.4"),
                             ("libc_archive_sha256", "invalid"),
                             ("production_binary_sha256", {name: "0" * 64 for name in hashes})):
            with self.subTest(field=field, value=value), self.assertRaises(canonical_audit.AuditError):
                canonical_audit.verify_linux_build_metadata(json.dumps({**metadata, field: value}).encode(),
                                                             hashes, "linux-musl-x86_64")

    def test_ifd_metadata_binds_source_architecture_libc_and_final_library_hash(self):
        digest = hashlib.sha256(b"stripped IFD").hexdigest()
        source_ref = "a" * 40
        metadata = {"schema": 1, "source_ref": source_ref, "compiler_target": "x86_64-linux-musl",
                    "library_sha256": digest}
        canonical_audit.verify_linux_ifd_metadata(json.dumps(metadata).encode(), digest,
                                                   "linux-musl-x86_64", source_ref)
        for field, value in (("source_ref", "b" * 40), ("library_sha256", "0" * 64),
                             ("compiler_target", "aarch64-linux-musl"),
                             ("compiler_target", "x86_64-linux-gnu")):
            with self.subTest(field=field), self.assertRaises(canonical_audit.AuditError):
                canonical_audit.verify_linux_ifd_metadata(json.dumps({**metadata, field: value}).encode(),
                                                           digest, "linux-musl-x86_64", source_ref)

    def test_flat_archives_are_reproducible_and_preserve_modes(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stage = root / "stage"
            stage.mkdir()
            canonical_package.write_text(stage / "asicend", "command\n")
            (stage / "asicend").chmod(0o755)
            canonical_package.write_text(stage / "notice", "notice\n")
            for writer, extension in ((canonical_package.deterministic_tar, "tar.gz"),
                                      (canonical_package.deterministic_zip, "zip")):
                first, second = root / f"first.{extension}", root / f"second.{extension}"
                writer(stage, first)
                writer(stage, second)
                self.assertEqual(first.read_bytes(), second.read_bytes())
            with tarfile.open(root / "first.tar.gz") as archive:
                self.assertEqual(set(archive.getnames()), {"asicend", "notice"})
                self.assertEqual(archive.getmember("asicend").mode, 0o755)
                self.assertEqual(archive.getmember("notice").mode, 0o644)

    def source_fixture(self, root, *, partial_windows=False):
        libusb = os.environ.get("ASICEN_LIBUSB_1_0_30_ARCHIVE")
        if not libusb:
            self.skipTest("set ASICEN_LIBUSB_1_0_30_ARCHIVE for pinned-source integration fixtures")
        source = root / "source"
        source.mkdir()
        required = (
            "LICENSE", "VERSION", "README.md", "THIRD_PARTY_NOTICES.md",
            "scripts/build-linux-static.sh", "scripts/build-linux-ifd.sh", "scripts/build-macos-static.sh",
            "scripts/test-static-relink.sh", "packaging/REBUILD.md.in", "packaging/DEPENDENCY-NOTICE.txt.in",
        )
        for name in required:
            canonical_package.copy_regular(SCRIPT.parent.parent / name, source / name)
        windows = canonical_audit.WINDOWS_SOURCE_REQUIRED
        if partial_windows:
            windows = {"repository/third_party/px4-userland/userland/src/windows/windows_ipc.cpp"}
        for name in windows:
            canonical_package.write_text(source / name.removeprefix("repository/"), "// synthetic source fixture\n")
        version = (source / "VERSION").read_text().strip()

        def snapshot(_source, _ref, stage):
            # Mock only the Git boundary: never add, commit, or modify the task repository.
            canonical_package.copy_tree(source, stage / "repository")
            return "a" * 40, "b" * 40

        arguments = ["package-source.py", "--version", version, "--source-root", str(source),
                     "--source-ref", "synthetic-fixture", "--libusb-source-archive", libusb,
                     "--output-dir", str(root / "out")]
        with mock.patch.object(sys, "argv", arguments), \
                mock.patch.object(canonical_source, "git_output", return_value="a" * 40), \
                mock.patch.object(canonical_source, "ref_version", return_value=version), \
                mock.patch.object(canonical_source, "add_git_snapshot", side_effect=snapshot), \
                contextlib.redirect_stdout(io.StringIO()):
            canonical_source.main()
        return root / "out" / f"asicen-userland-{version}-source.tar.gz"

    def test_pinned_source_archive_and_expanded_libusb_integrity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            archive = self.source_fixture(root)
            canonical_audit.audit_source_archive(argparse.Namespace(archive=archive))
            stage = root / "expanded"
            stage.mkdir()
            with tarfile.open(archive) as stream:
                self.assertFalse(any("firmware/" in name for name in stream.getnames()))
                stream.extractall(stage, filter="data")
            source_file = stage / "third_party/libusb-1.0.30/libusb/core.c"
            source_file.write_bytes(source_file.read_bytes() + b"\n/* tampered source */\n")
            manifest_path = stage / "source-manifest.json"
            manifest = json.loads(manifest_path.read_text())
            manifest["files"][source_file.relative_to(stage).as_posix()] = {
                "size": source_file.stat().st_size, "sha256": canonical_source.sha256(source_file),
            }
            manifest_path.write_text(json.dumps(manifest) + "\n")
            canonical_source.write_checksums(stage)
            altered = root / "altered" / archive.name
            altered.parent.mkdir()
            canonical_source.deterministic_tar(stage, altered)
            with self.assertRaisesRegex(canonical_audit.AuditError, "expanded libusb source differs"):
                canonical_audit.audit_source_archive(argparse.Namespace(archive=altered))

    def test_partial_windows_source_is_rejected_without_creating_commits(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(canonical_source.AuditError, "source archive is missing"):
                self.source_fixture(Path(temporary), partial_windows=True)


if __name__ == "__main__":
    unittest.main()
