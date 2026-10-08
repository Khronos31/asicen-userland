#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
import importlib.util
import io
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "audit-linux-candidate.py"
spec = importlib.util.spec_from_file_location("asicen_distribution_audit", SCRIPT)
audit = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(audit)
PACKAGE_SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "package-linux-variant.py"
package_spec = importlib.util.spec_from_file_location("asicen_linux_package", PACKAGE_SCRIPT)
package = importlib.util.module_from_spec(package_spec)
assert package_spec.loader is not None
package_spec.loader.exec_module(package)


class StaticDistributionAuditTests(unittest.TestCase):
    def test_source_archive_rejects_vendor_firmware(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                payload = b"not actual firmware"
                info = tarfile.TarInfo("firmware/asicen-loader.bin")
                info.size = len(payload)
                archive.addfile(info, io.BytesIO(payload))
            with self.assertRaisesRegex(ValueError, "firmware"):
                audit.audit_source_archive(path)

    def test_source_archive_rejects_path_traversal(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "source.tar.gz"
            with tarfile.open(path, "w:gz") as archive:
                payload = b"x"
                info = tarfile.TarInfo("../outside")
                info.size = 1
                archive.addfile(info, io.BytesIO(payload))
            with self.assertRaisesRegex(ValueError, "unsafe path"):
                audit.audit_source_archive(path)

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
            header = audit.run_readelf("-h", executable)
            self.assertIn("Machine:", header)
            self.assertNotIn("INTERP", audit.run_readelf("-l", executable))
            self.assertNotIn("NEEDED", audit.run_readelf("-d", executable))
            candidate = root / "candidate"
            candidate.mkdir()
            (candidate / "BUILD-METADATA.txt").write_text(
                "Candidate class: intermediate Linux static command build; incomplete, not a release.\n"
                "Compiler target: x86_64-linux-gnu\n"
                "Musl libc archive identification: glibc static archive\n",
                encoding="utf-8")
            (candidate / "SHA256SUMS").write_text("", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "musl compiler target"):
                audit.audit_candidate(candidate)

    def test_candidate_root_symlink_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            real = root / "real"
            real.mkdir()
            link = root / "candidate"
            link.symlink_to(real, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "real directory"):
                audit.audit_candidate(link)

    def test_corresponding_source_bundle_accepts_directory_entries(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "candidate"
            (root / "source").mkdir(parents=True)
            (root / "licenses").mkdir()
            files = {
                "source/asicen-userland-source.tar.gz": b"source",
                "source/libusb-1.0.30.tar.bz2": b"libusb",
                "SOURCE-MANIFEST.txt": b"manifest\n",
                "REBUILD.md": b"rebuild\n",
                "licenses/notice.txt": b"notice\n",
            }
            for relative, payload in files.items():
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(payload)
            bundle = root / "source/bundle.tar.gz"
            with tarfile.open(bundle, "w:gz") as archive:
                for relative in ("source", "licenses"):
                    archive.add(root / relative, arcname=relative, recursive=False)
                for relative in files:
                    archive.add(root / relative, arcname=relative)
            audit.audit_corresponding_source_bundle(bundle, root)
            (root / "licenses/notice.txt").write_bytes(b"changed\n")
            with self.assertRaisesRegex(ValueError, "does not match"):
                audit.audit_corresponding_source_bundle(bundle, root)

    def test_link_map_normalizes_relative_compiler_archive_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            base = root / "build"
            base.mkdir()
            libc = root / "toolchain/lib/libc.a"
            libc.parent.mkdir(parents=True)
            libc.write_bytes(b"libc archive")
            map_file = root / "link.map"
            map_file.write_text("LOAD ../toolchain/lib/./libc.a\n", encoding="utf-8")
            audit.verify_link_map(map_file, libc, base)
            map_file.write_text("LOAD ../toolchain/lib/not-libc.a\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "does not show"):
                audit.verify_link_map(map_file, libc, base)

    def test_candidate_modes_and_archive_are_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "candidate"
            (root / "bin").mkdir(parents=True)
            (root / "licenses").mkdir()
            (root / "bin/asicend").write_bytes(b"command")
            (root / "licenses/notice").write_bytes(b"notice")
            audit.normalize_candidate_modes(root)
            self.assertEqual((root.stat().st_mode & 0o777), 0o755)
            self.assertEqual((root / "bin/asicend").stat().st_mode & 0o777, 0o755)
            self.assertEqual((root / "licenses/notice").stat().st_mode & 0o777, 0o644)
            (root / "licenses/notice").chmod(0o600)
            with self.assertRaisesRegex(ValueError, "mode mismatch"):
                audit.audit_candidate_modes(root)
            audit.normalize_candidate_modes(root)
            first = Path(temporary) / "first.tar.gz"
            second = Path(temporary) / "second.tar.gz"
            package.write_reproducible_archive(root, first)
            package.write_reproducible_archive(root, second)
            self.assertEqual(package.digest(first), package.digest(second))

    def test_final_inventory_is_variant_specific_and_closed(self):
        glibc = package.expected_final_inventory("glibc")
        musl = package.expected_final_inventory("musl")
        self.assertNotIn("licenses/IFD-musl-COPYRIGHT.txt", glibc)
        self.assertIn("licenses/IFD-musl-COPYRIGHT.txt", musl)
        self.assertIn("firmware/asicen-loader.bin", glibc)
        self.assertIn("lib/pcsc/ifd-asicen.so", musl)
        self.assertNotIn("firmware/extra.bin", musl)

    def test_retained_build_metadata_must_match_source_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            metadata = Path(temporary) / "BUILD-METADATA.txt"
            metadata.write_text("Source archive SHA-256: " + "a" * 64 + "\n",
                                encoding="utf-8")
            package.require_source_metadata(metadata, "a" * 64)
            with self.assertRaisesRegex(ValueError, "does not match"):
                package.require_source_metadata(metadata, "b" * 64)

    def _ifd_fixture(self, directory, variant="glibc-2.31", source_sha="a" * 64):
        plugin = directory / "libifd-asicen.so"
        plugin.write_bytes(b"fixture ELF placeholder")
        metadata = directory / "BUILD-METADATA.txt"
        metadata.write_text(
            f"Libc variant: {variant}\nSource archive SHA-256: {source_sha}\n",
            encoding="utf-8")
        (directory / "SHA256SUMS").write_text(
            f"{package.digest(plugin)}  libifd-asicen.so\n"
            f"{package.digest(metadata)}  BUILD-METADATA.txt\n", encoding="ascii")

    def test_ifd_rejects_wrong_libc_and_source_snapshot(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self._ifd_fixture(directory, variant="musl")
            with self.assertRaisesRegex(ValueError, "does not match requested"):
                package.validate_ifd(directory, "glibc", "a" * 64,
                                     "Advanced Micro Devices X86-64")
            self._ifd_fixture(directory, source_sha="b" * 64)
            with self.assertRaisesRegex(ValueError, "same immutable source"):
                package.validate_ifd(directory, "glibc", "a" * 64,
                                     "Advanced Micro Devices X86-64")

    def test_ifd_rejects_wrong_architecture_and_missing_exports(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self._ifd_fixture(directory)
            with mock.patch.object(package, "machine_of", return_value="Advanced Micro Devices X86-64"):
                with self.assertRaisesRegex(ValueError, "architecture"):
                    package.validate_ifd(directory, "glibc", "a" * 64, "AArch64")
            def fake_run(args, **kwargs):
                output = ""
                if args[:2] == ["readelf", "-d"]:
                    output = ("0x00000001 (NEEDED) Shared library: [libc.so.6]\n"
                              "Shared library: [libpthread.so.0]\n"
                              "Shared library: [ld-linux-x86-64.so.2]\n")
                return subprocess.CompletedProcess(args, 0, stdout=output)
            with mock.patch.object(package, "machine_of", return_value="Advanced Micro Devices X86-64"), \
                 mock.patch.object(package.subprocess, "run", side_effect=fake_run):
                with self.assertRaisesRegex(ValueError, "IFD export missing"):
                    package.validate_ifd(directory, "glibc", "a" * 64,
                                         "Advanced Micro Devices X86-64")

    def test_firmware_inputs_and_final_inventory_are_guarded(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            repo = root / "repo"
            repo.mkdir()
            fw = repo / "firmware.bin"
            fw.write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "outside the source checkout"):
                package.validate_firmware_path(fw, repo)
            external = root / "external.bin"
            external.write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "outside the source checkout"):
                package.validate_firmware_path(external, root)
            alias = root / "alias.bin"
            alias.symlink_to(external)
            with self.assertRaisesRegex(ValueError, "regular non-symlink"):
                package.validate_firmware_path(alias, repo)
            with self.assertRaisesRegex(ValueError, "vendor firmware notice"):
                package.validate_firmware_notice(root / "missing-notice")
            bad_notice = root / "bad-notice.txt"
            bad_notice.write_text("vendor notice without expected hash or rights status\n")
            with self.assertRaisesRegex(ValueError, "rights status"):
                package.validate_firmware_notice(bad_notice)
            required = [
                "firmware/asicen-loader.bin", "licenses/VENDOR-FIRMWARE-NOTICE.txt",
                "licenses/COPYING.gpl2", "licenses/px4-userland-GPL-2.0-only.txt",
                "licenses/libusb-COPYING", "licenses/NOTICES.md",
                "licenses/musl-COPYRIGHT.txt", "licenses/GCC-COPYING3.txt",
                "licenses/GCC-COPYING.RUNTIME.txt", "licenses/IFD-BUILD-METADATA.txt",
                "licenses/COMMAND-BUILD-METADATA.txt", "licenses/IFD-GCC-COPYING3.txt",
                "licenses/IFD-GCC-COPYING.RUNTIME.txt", "pcsc/reader.conf.d/asicen-reader.conf.in",
                "lib/pcsc/ifd-asicen.so",
            ]
            for relative in required:
                if relative == "licenses/IFD-GCC-COPYING.RUNTIME.txt":
                    continue
                path = root / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"fixture\n")
            with self.assertRaisesRegex(ValueError, "IFD-GCC-COPYING.RUNTIME.txt"):
                package.require_final_files(root)
            with self.assertRaisesRegex(ValueError, "outside the source checkout"):
                package.validate_output_path(root / "candidate", root)
            outside = root / "candidate-forbidden-existing"
            outside.write_text("existing\n")
            try:
                with self.assertRaisesRegex(ValueError, "already exists"):
                    package.validate_output_path(outside, root)
            finally:
                outside.unlink()


if __name__ == "__main__":
    unittest.main()
