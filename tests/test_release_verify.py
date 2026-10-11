#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic, offline release gate regressions. No vendor code is run."""

import copy
import gzip
import importlib.util
import io
import json
from pathlib import Path
import shutil
import stat
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile

SPEC = importlib.util.spec_from_file_location(
    "release_verify", Path(__file__).resolve().parents[1] / "scripts/release_verify.py")
verify = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = verify
SPEC.loader.exec_module(verify)

SHA, TREE, TAG_SHA, TAG_TREE = "a" * 40, "b" * 40, "c" * 40, "d" * 40
VERSION, TAG = "1.2.3", "v1.2.3"
FIRMWARE = b"synthetic-test!\0" * 1024  # No vendor firmware fixture.
assert len(FIRMWARE) == 16384


def encode(value):
    return (json.dumps(value, indent=2, sort_keys=True) + "\n").encode()


def add_checksums(files):
    files["SHA256SUMS"] = ("".join(
        f"{verify.digest(data)}  {name}\n" for name, (data, _) in sorted(files.items())
        if name != "SHA256SUMS").encode(), 0o644)


def refresh_manifest(files, source=False):
    name = "source-manifest.json" if source else "manifest.json"
    manifest = json.loads(files[name][0])
    manifest["files"] = {n: {"size": len(data), "sha256": verify.digest(data)}
                         for n, (data, _) in files.items()
                         if n not in {name, "SHA256SUMS"}}
    files[name] = (encode(manifest), files[name][1])
    add_checksums(files)


def write_archive(path, files):
    if path.name.endswith(".zip"):
        with zipfile.ZipFile(path, "w") as archive:
            for name, (data, mode) in sorted(files.items()):
                info = zipfile.ZipInfo(name)
                info.create_system = 3
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (stat.S_IFREG | mode) << 16
                archive.writestr(info, data)
    else:
        with path.open("wb") as target:
            with gzip.GzipFile(fileobj=target, mode="wb", mtime=0, filename="") as raw:
                with tarfile.open(fileobj=raw, mode="w") as archive:
                    for name, (data, mode) in sorted(files.items()):
                        info = tarfile.TarInfo(name)
                        info.mode, info.size = mode, len(data)
                        archive.addfile(info, io.BytesIO(data))


class ReleaseFixture:
    def __init__(self, root, product="px4-userland", repository=None):
        self.root, self.product = root, product
        self.repository = repository or "fixture/" + product
        self.candidate, self.reproduction, self.tag_dir = [root / n for n in ("candidate", "repro", "tag")]
        for directory in (self.candidate, self.reproduction, self.tag_dir):
            directory.mkdir(parents=True)
        self.notes = f"# {product} {TAG}\n\n公開用の概要です。\n".encode()
        self.record_path = root / "record.json"
        self.files = {}
        self.build_bundle(self.candidate, SHA, TREE, b"Candidate README\n", b"pending\n", b"{}\n")
        for path in self.candidate.iterdir():
            shutil.copyfile(path, self.reproduction / path.name)
        hashes = verify.verify_bundle(self.candidate, product, VERSION)
        candidate = {"run_id": 100, "run_attempt": 1, "artifact_id": 1000, "sha": SHA,
                     "tree": TREE, "workflow": verify.WORKFLOW, "artifact_name": "release-candidate",
                     "assets_sha256": hashes, "build_inputs": {"compiler": "fixture-v1"},
                     "runner_images": {"ubuntu": "fixture-image-1"}}
        reproduction = copy.deepcopy(candidate)
        reproduction.update(run_id=101, artifact_id=1001, runner_images={"ubuntu": "fixture-image-2"})
        completed = {"status": "completed", "evidence": ["docs/platforms/validation-results.md#release"]}
        gates = {name: copy.deepcopy(completed) for name in (
            "specification_tests", "soak_decision", "support_review", "prepublication_review")}
        gates["hardware"] = {p: copy.deepcopy(completed) for p in verify.platforms(product)}
        self.record = {"schema": 1, "product": product, "repository": self.repository,
                       "version": VERSION, "notes_sha256": verify.digest(self.notes),
                       "candidate": candidate, "reproduction": reproduction, "gates": gates}
        self.record_path.write_bytes(encode(self.record))
        self.build_bundle(self.tag_dir, TAG_SHA, TAG_TREE, b"Reviewed README\n", self.notes, encode(self.record))

    def source_files(self, sha, tree, readme, notes, record):
        template = (b"# Rebuilding @VERSION@\n\ncommit `@COMMIT@`\ntree `@TREE@`\n"
                    b"Run scripts/build.sh without changing these instructions.\n")
        files = {
            "README.md": (readme, 0o644), "repository/README.md": (readme, 0o644),
            "repository/VERSION": ((VERSION + "\n").encode(), 0o644),
            "repository/SPEC.md": (b"Specification\n", 0o644),
            "repository/docs/platforms/validation-results.md": (b"# release\nActual evidence lives here.\n", 0o644),
            "repository/HARDWARE-VALIDATION.md": (b"# release\nHistorical ASICEN evidence.\n", 0o644),
            f"repository/release-notes/{TAG}.md": (notes, 0o644),
            f"repository/release-notes/{TAG}.json": (record, 0o644),
            "repository/packaging/REBUILD.md.in": (template, 0o644),
            "repository/scripts/build.sh": (b"#!/bin/sh\necho build\n", 0o755),
            "repository/userland/main.cpp": (b"int main() { return 0; }\n", 0o644),
            "third_party/libusb-1.0.30/COPYING": (b"fixture license\n", 0o664),
            "third_party/libusb-1.0.30/configure": (b"fixture configure\n", 0o775),
            "BUILD-RELINK.md": (template.replace(b"@VERSION@", VERSION.encode()).replace(
                b"@COMMIT@", sha.encode()).replace(b"@TREE@", tree.encode()), 0o644),
        }
        manifest = {"schema": 1, "version": VERSION, "kind": "corresponding-source",
                    "repository_commit": sha, "repository_tree": tree, "libusb_version": "1.0.30",
                    "libusb_archive_sha256": verify.LIBUSB_SHA256,
                    "forbidden_materials_excluded": ["firmware"], "files": {}}
        files["source-manifest.json"] = (encode(manifest), 0o644)
        refresh_manifest(files, source=True)
        return files

    def binary_files(self, platform, sha, readme):
        programs = ["px4d", "px4-ts", "px4ctl"] if self.product == "px4-userland" else [
            "asicend", "asicen-ts", "asicenctl"]
        files = {name + (".exe" if platform == "windows-x86_64" else ""):
                 (b"synthetic executable " + name.encode(), 0o755) for name in programs}
        files.update({"README.md": (readme, 0o644), "LICENSE": (b"fixture license", 0o644),
                      "libusb/COPYING": (b"fixture libusb license", 0o644),
                      "THIRD_PARTY_NOTICES.md": (b"fixture notice", 0o644),
                      "evidence/binary-audit.json": (b'{"audit":"fixture"}\n', 0o644)})
        if platform.startswith("android-"):
            files["px4-termux" if self.product == "px4-userland" else "asicen-termux"] = (
                b"#!/bin/sh\necho launcher\n", 0o755)
        if self.product == "asicen-userland":
            files[verify.FIRMWARE_MEMBER] = (FIRMWARE, 0o644)
            files["licenses/VENDOR-FIRMWARE-NOTICE.txt"] = (b"fixture vendor notice", 0o644)
            if platform.startswith("linux-"):
                files["ifd/asicen-userland-ifd.so"] = (b"synthetic IFD", 0o755)
                arch = platform.rsplit("-", 1)[-1]
                static = {"schema": 1, "source_ref": sha, "compiler_target": arch + "-linux-musl",
                          "libc_archive_sha256": "e" * 64, "libc_identification": "musl 1.2.5",
                          "production_binary_sha256": {p: verify.digest(files[p][0]) for p in programs}}
                ifd = {"schema": 1, "source_ref": sha, "compiler_target": arch + "-linux-" + (
                    "musl" if platform.startswith("linux-musl-") else "gnu"),
                       "library_sha256": verify.digest(files["ifd/asicen-userland-ifd.so"][0])}
                files["evidence/linux-static-build.json"] = (encode(static), 0o644)
                files["evidence/linux-ifd-build.json"] = (encode(ifd), 0o644)
        libc = ("glibc" if platform.startswith("linux-glibc-") else
                "musl" if platform.startswith("linux-musl-") else
                "android" if platform.startswith("android-") else
                "ucrt" if platform == "windows-x86_64" else "darwin")
        manifest = {"schema": 1, "version": VERSION, "platform": platform, "libc": libc,
                    "architecture": platform.rsplit("-", 1)[-1],
                    "embedded_libusb": {"version": "1.0.30", "linkage": "static"},
                    "source_ref": sha, "programs": programs, "production_only": True, "files": {}}
        files["manifest.json"] = (encode(manifest), 0o644)
        refresh_manifest(files)
        return files

    def build_bundle(self, directory, sha, tree, readme, notes, record):
        for platform in verify.platforms(self.product):
            extension = "zip" if platform == "windows-x86_64" else "tar.gz"
            name = f"{self.product}-{VERSION}-{platform}.{extension}"
            files = self.binary_files(platform, sha, readme)
            self.files[(directory, name)] = files
            write_archive(directory / name, files)
        name = f"{self.product}-{VERSION}-source.tar.gz"
        files = self.source_files(sha, tree, readme, notes, record)
        self.files[(directory, name)] = files
        write_archive(directory / name, files)
        self.outer(directory)

    @staticmethod
    def outer(directory):
        (directory / "SHA256SUMS").write_text("".join(
            f"{verify.file_digest(p)}  {p.name}\n" for p in sorted(directory.iterdir())
            if p.name != "SHA256SUMS"), encoding="ascii")

    def mutate(self, member, data=None, *, platform="linux-glibc-x86_64", mode=None,
               remove=False, refresh=True, source=False):
        extension = "zip" if platform == "windows-x86_64" else "tar.gz"
        name = f"{self.product}-{VERSION}-" + ("source.tar.gz" if source else f"{platform}.{extension}")
        files = self.files[(self.tag_dir, name)]
        if remove:
            del files[member]
        else:
            old_data, old_mode = files.get(member, (b"", 0o644))
            files[member] = (old_data if data is None else data, old_mode if mode is None else mode)
        if refresh:
            refresh_manifest(files, source=source)
        else:
            add_checksums(files)
        write_archive(self.tag_dir / name, files)
        self.outer(self.tag_dir)

    def run(self):
        return verify.verify_release(self.record_path, self.candidate, self.reproduction, self.tag_dir,
                                     product=self.product, tag=TAG, tag_sha=TAG_SHA, tag_tree=TAG_TREE,
                                     repository=self.repository)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.synthetic_firmware = patch.object(verify, "FIRMWARE_SHA256", verify.digest(FIRMWARE))
        self.synthetic_firmware.start()

    def tearDown(self):
        self.synthetic_firmware.stop()
        self.temp.cleanup()

    def fixture(self, product="px4-userland"):
        return ReleaseFixture(self.root / product, product)

    def test_both_profiles_document_only_and_real_source_modes(self):
        for product, asset_count in (("px4-userland", 11), ("asicen-userland", 10)):
            with self.subTest(product=product):
                fixture = self.fixture(product)
                result = fixture.run()
                self.assertTrue(result["verified"])
                self.assertEqual(asset_count, len(result["assets_sha256"]))
                self.assertEqual(100, result["candidate_run_id"])
                self.assertEqual(101, result["reproduction_run_id"])
                self.assertEqual(product == "px4-userland", any(
                    "windows" in name for name in result["assets_sha256"]))

    def test_all_binary_platform_payloads_and_windows_are_protected(self):
        for index, platform in enumerate(verify.platforms("px4-userland")):
            with self.subTest(platform=platform):
                fixture = ReleaseFixture(self.root / str(index))
                filename = "px4d.exe" if platform == "windows-x86_64" else "px4d"
                fixture.mutate(filename, b"changed executable", platform=platform)
                with self.assertRaisesRegex(verify.VerificationError, "payload changed"):
                    fixture.run()

    def test_payload_modes_inventory_launchers_notices_and_audit(self):
        attacks = [
            ("px4d", None, {"mode": 0o644}),
            ("extra.txt", b"extra", {}),
            ("LICENSE", None, {"remove": True}),
            ("THIRD_PARTY_NOTICES.md", b"changed notice", {}),
            ("evidence/binary-audit.json", b'{"audit":"changed"}', {}),
            ("px4-termux", b"changed launcher", {"platform": "android-aarch64"}),
        ]
        for i, (member, data, kwargs) in enumerate(attacks):
            with self.subTest(member=member):
                fixture = ReleaseFixture(self.root / str(i))
                fixture.mutate(member, data, **kwargs)
                with self.assertRaises(verify.VerificationError):
                    fixture.run()

    def test_source_scripts_sources_vendor_material_and_rebuild_protected(self):
        attacks = [
            ("repository/scripts/build.sh", b"malicious script"),
            ("repository/userland/main.cpp", b"changed source"),
            ("third_party/libusb-1.0.30/COPYING", b"changed third party license"),
            ("BUILD-RELINK.md", b"commit " + TAG_SHA.encode() + b"\nchanged instructions"),
            ("repository/new-doc.md", b"even added docs change inventory"),
            ("repository/firmware/hidden.bin", b"firmware"),
            ("repository/cache.pyc", b"bytecode"),
        ]
        for i, (member, data) in enumerate(attacks):
            with self.subTest(member=member):
                fixture = ReleaseFixture(self.root / str(i))
                fixture.mutate(member, data, source=True)
                with self.assertRaises(verify.VerificationError):
                    fixture.run()

    def test_allowed_source_document_can_change(self):
        fixture = self.fixture()
        fixture.mutate("repository/SPEC.md", b"Recorded acceptance evidence\n", source=True)
        self.assertTrue(fixture.run()["verified"])

    def test_readme_must_match_source_snapshot(self):
        fixture = self.fixture()
        fixture.mutate("README.md", b"arbitrary replacement README\n")
        with self.assertRaisesRegex(verify.VerificationError, "README"):
            fixture.run()

    def test_manifest_fields_are_not_blanket_ignored(self):
        for i, (field, value) in enumerate((
                ("source_ref", "e" * 40), ("libc", "wrong"), ("new_metadata", "attack"),
                ("production_only", False), ("version", "99.9.9"))):
            with self.subTest(field=field):
                fixture = ReleaseFixture(self.root / str(i))
                name = f"px4-userland-{VERSION}-linux-glibc-x86_64.tar.gz"
                manifest = json.loads(fixture.files[(fixture.tag_dir, name)]["manifest.json"][0])
                manifest[field] = value
                fixture.mutate("manifest.json", encode(manifest))
                with self.assertRaises(verify.VerificationError):
                    fixture.run()

    def test_manifest_file_hash_size_and_inventory_must_match_bytes(self):
        for i, attack in enumerate(("hash", "size", "bool-size", "missing", "extra")):
            with self.subTest(attack=attack):
                fixture = ReleaseFixture(self.root / str(i))
                name = f"px4-userland-{VERSION}-linux-glibc-x86_64.tar.gz"
                manifest = json.loads(fixture.files[(fixture.tag_dir, name)]["manifest.json"][0])
                if attack == "hash":
                    manifest["files"]["README.md"]["sha256"] = "f" * 64
                elif attack in {"size", "bool-size"}:
                    manifest["files"]["README.md"]["size"] = True if attack == "bool-size" else 1
                elif attack == "missing":
                    del manifest["files"]["LICENSE"]
                else:
                    manifest["files"]["extra"] = {"size": 0, "sha256": "f" * 64}
                fixture.mutate("manifest.json", encode(manifest), refresh=False)
                with self.assertRaisesRegex(verify.VerificationError, "manifest"):
                    fixture.run()

    def test_asicen_firmware_notice_and_source_exclusion(self):
        for i, (member, data, source) in enumerate((
                (verify.FIRMWARE_MEMBER, b"changed firmware", False),
                ("licenses/VENDOR-FIRMWARE-NOTICE.txt", b"rights granted falsely", False),
                ("repository/firmware/asicen-loader.bin", FIRMWARE, True))):
            with self.subTest(member=member):
                fixture = ReleaseFixture(self.root / str(i), "asicen-userland")
                fixture.mutate(member, data, source=source)
                with self.assertRaises(verify.VerificationError):
                    fixture.run()

    def test_asicen_provenance_exception_only_changes_exact_source_ref(self):
        for i, (member, field, value) in enumerate((
                ("evidence/linux-static-build.json", "source_ref", "f" * 40),
                ("evidence/linux-static-build.json", "libc_identification", "changed build"),
                ("evidence/linux-ifd-build.json", "library_sha256", "f" * 64),
                ("evidence/linux-ifd-build.json", "compiler_target", "x86_64-linux-other"))):
            with self.subTest(member=member, field=field):
                fixture = ReleaseFixture(self.root / str(i), "asicen-userland")
                name = f"asicen-userland-{VERSION}-linux-glibc-x86_64.tar.gz"
                value_json = json.loads(fixture.files[(fixture.tag_dir, name)][member][0])
                value_json[field] = value
                fixture.mutate(member, encode(value_json))
                with self.assertRaises(verify.VerificationError):
                    fixture.run()

    def test_checksum_files_enforce_exact_inventory_and_digest(self):
        fixture = self.fixture()
        name = f"px4-userland-{VERSION}-linux-glibc-x86_64.tar.gz"
        checksum = fixture.files[(fixture.tag_dir, name)]["SHA256SUMS"][0]
        for value in (checksum + checksum.splitlines(keepends=True)[0], checksum.replace(b"README.md", b"other.md"),
                      b"f" * 64 + b"  README.md\n", checksum.replace(b"\n", b"\r\n")):
            with self.subTest(value=value[:75]):
                fixture.files[(fixture.tag_dir, name)]["SHA256SUMS"] = (value, 0o644)
                write_archive(fixture.tag_dir / name, fixture.files[(fixture.tag_dir, name)])
                fixture.outer(fixture.tag_dir)
                with self.assertRaises(verify.VerificationError):
                    fixture.run()

    def test_outer_assets_extra_missing_symlink_and_checksum_tampering(self):
        fixture = self.fixture()
        original = (fixture.tag_dir / "SHA256SUMS").read_bytes()
        (fixture.tag_dir / "SHA256SUMS").write_bytes(original + original.splitlines(keepends=True)[0])
        with self.assertRaises(verify.VerificationError):
            fixture.run()
        (fixture.tag_dir / "SHA256SUMS").write_bytes(original)
        (fixture.tag_dir / "extra.zip").write_bytes(b"extra")
        with self.assertRaisesRegex(verify.VerificationError, "inventory"):
            fixture.run()
        (fixture.tag_dir / "extra.zip").unlink()
        archive = next(fixture.tag_dir.glob("*.tar.gz"))
        saved = self.root / "saved.tar.gz"
        archive.rename(saved)
        with self.assertRaisesRegex(verify.VerificationError, "inventory"):
            fixture.run()
        archive.symlink_to(saved)
        with self.assertRaisesRegex(verify.VerificationError, "regular file"):
            fixture.run()

    def test_pinned_candidate_and_reproduction_bytes_are_required(self):
        for i, label in enumerate(("candidate", "reproduction")):
            fixture = ReleaseFixture(self.root / str(i))
            directory = getattr(fixture, label)
            archive = next(directory.glob("*.tar.gz"))
            with archive.open("ab") as stream:
                stream.write(b"different compressed bytes")
            fixture.outer(directory)
            with self.assertRaisesRegex(verify.VerificationError, "pinned record"):
                fixture.run()

    def test_record_gates_and_identities_fail_closed(self):
        fixture = self.fixture()
        base = fixture.record
        mutations = [
            lambda r: r["candidate"].update(run_id=True),
            lambda r: r["candidate"].update(artifact_id=0),
            lambda r: r["candidate"].update(workflow="different.yml"),
            lambda r: r["candidate"].update(sha="short"),
            lambda r: r["reproduction"].update(run_id=r["candidate"]["run_id"]),
            lambda r: r["reproduction"].update(artifact_id=r["candidate"]["artifact_id"]),
            lambda r: r["reproduction"]["build_inputs"].update(compiler="different"),
            lambda r: r["candidate"].update(runner_images={}),
            lambda r: r["gates"]["hardware"].pop("windows-x86_64"),
            lambda r: r["gates"]["soak_decision"].update(status="pending"),
            lambda r: r["gates"]["support_review"].update(evidence=[]),
            lambda r: r["gates"]["support_review"].update(evidence=["../secret#evidence"]),
            lambda r: r["gates"]["support_review"].update(evidence=["SPEC.md"]),
            lambda r: r.update(repository="other/project"),
            lambda r: r.update(extra=True),
        ]
        for mutation in mutations:
            value = copy.deepcopy(base)
            mutation(value)
            with self.assertRaises(verify.VerificationError):
                verify.validate_record(value, product=fixture.product, tag=TAG,
                                       repository="fixture/px4-userland")

    def test_record_and_notes_must_be_committed_exact_tag_bytes(self):
        fixture = self.fixture()
        fixture.mutate(f"repository/release-notes/{TAG}.md", b"different notes", source=True)
        with self.assertRaisesRegex(verify.VerificationError, "notes"):
            fixture.run()
        fixture.mutate(f"repository/release-notes/{TAG}.md", fixture.notes, source=True)
        fixture.mutate(f"repository/release-notes/{TAG}.json", b"{}\n", source=True)
        with self.assertRaisesRegex(verify.VerificationError, "record differs"):
            fixture.run()

    def test_source_identity_cannot_be_arbitrary_even_in_allowed_manifest(self):
        fixture = self.fixture()
        name = f"px4-userland-{VERSION}-source.tar.gz"
        manifest = json.loads(fixture.files[(fixture.tag_dir, name)]["source-manifest.json"][0])
        manifest["repository_tree"] = "f" * 40
        fixture.mutate("source-manifest.json", encode(manifest), source=True)
        with self.assertRaisesRegex(verify.VerificationError, "commit/tree"):
            fixture.run()


class ArchiveSafetyTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def tar(self, infos):
        path = self.root / "attack.tar.gz"
        with tarfile.open(path, "w:gz") as archive:
            for info, data in infos:
                archive.addfile(info, io.BytesIO(data))
        return path

    def test_tar_traversal_duplicates_links_devices_and_case_collision(self):
        cases = [(name, tarfile.REGTYPE) for name in (
            "../escape", "/absolute", "a/../b", "a//b", "a/./b", "C:/path", "a\\b", "a\nname")]
        cases += [("link", t) for t in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE, tarfile.DIRTYPE)]
        for name, kind in cases:
            with self.subTest(name=name, kind=kind):
                info = tarfile.TarInfo(name)
                info.type, info.linkname = kind, "../escape"
                with self.assertRaises(verify.VerificationError):
                    verify.read_archive(self.tar([(info, b"")]))
        for names in (("same", "same"), ("File", "file"), ("parent", "parent/child")):
            with self.assertRaises(verify.VerificationError):
                verify.read_archive(self.tar([(tarfile.TarInfo(n), b"") for n in names]))

    def test_tar_mode_expansion_member_count_and_size_limits(self):
        info = tarfile.TarInfo("payload")
        info.mode = 0o4755
        with self.assertRaisesRegex(verify.VerificationError, "mode"):
            verify.read_archive(self.tar([(info, b"")]))
        info.mode, info.size = 0o644, 2048
        path = self.tar([(info, b"a" * 2048)])
        for limits in (verify.Limits(member_bytes=1024), verify.Limits(total_bytes=1024),
                       verify.Limits(archive_bytes=1), verify.Limits(members=0)):
            with self.assertRaises(verify.VerificationError):
                verify.read_archive(path, limits=limits)

    def test_tar_extra_nonzero_trailer_is_not_ignored(self):
        path = self.tar([(tarfile.TarInfo("payload"), b"")])
        with path.open("ab") as out:
            out.write(gzip.compress(b"hidden archive contents"))
        with self.assertRaises(verify.VerificationError):
            verify.read_archive(path)

    def test_zip_symlinks_modes_duplicates_paths_and_bombs(self):
        path = self.root / "attack.zip"
        for name, mode in (("../escape", stat.S_IFREG | 0o644),
                           ("link", stat.S_IFLNK | 0o777), ("device", stat.S_IFCHR | 0o644),
                           ("setuid", stat.S_IFREG | 0o4755)):
            with self.subTest(name=name):
                with zipfile.ZipFile(path, "w") as archive:
                    info = zipfile.ZipInfo(name)
                    info.create_system, info.external_attr = 3, mode << 16
                    archive.writestr(info, b"data")
                with self.assertRaises(verify.VerificationError):
                    verify.read_archive(path)
        write_archive(path, {"payload": (b"0" * 5000, 0o644)})
        with self.assertRaises(verify.VerificationError):
            verify.read_archive(path, limits=verify.Limits(member_bytes=1024))

    def test_duplicate_json_keys_and_nonfinite_constants_rejected(self):
        for value in (b'{"same":1,"same":2}', b'{"v":NaN}', b'{"v":Infinity}', b"[]"):
            with self.assertRaises(verify.VerificationError):
                verify.load_json(value, "fixture")


if __name__ == "__main__":
    unittest.main()
