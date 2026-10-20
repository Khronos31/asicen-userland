#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline release gate shared by PX4 and ASICEN.

No network, extraction, executable invocation, hardware operation, or publication
occurs here. CI remains responsible for platform-specific binary/license audits.
The committed record is a human-reviewed attestation, not proof that hardware
tests were performed. This verifier checks its identities and referenced bytes.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import gzip
import hashlib
import json
from pathlib import Path
import re
import stat
import sys
import tarfile
import zipfile


class VerificationError(ValueError):
    """A release gate failed; publication must stop."""


PLATFORMS = (
    "linux-glibc-x86_64", "linux-musl-x86_64", "linux-glibc-aarch64",
    "linux-musl-aarch64", "darwin-arm64", "android-aarch64",
    "android-armv7a", "android-x86_64",
)
PROFILES = {
    "px4-userland": PLATFORMS + ("windows-x86_64",),
    "asicen-userland": PLATFORMS,
}
DOCUMENTS = frozenset({
    "README.md", "SPEC.md", "docs/platforms/validation-results.md",
    "docs/release-validation.md", "docs/releasing.md",
})
ASICEN_PROVENANCE = frozenset({
    "evidence/linux-static-build.json", "evidence/linux-ifd-build.json",
})
WORKFLOW = ".github/workflows/build_userland.yml"
LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
FIRMWARE_MEMBER = "firmware/asicen-loader.bin"
FIRMWARE_SHA256 = "b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8"
HEX40 = re.compile(r"[0-9a-f]{40}\Z")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")
VERSION = re.compile(r"(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\Z")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def file_digest(path: Path) -> str:
    require(path.is_file() and not path.is_symlink(), f"not a regular file: {path}")
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()


def load_json(data: bytes, name: str) -> dict:
    def pairs(items: list) -> dict:
        result = {}
        for key, value in items:
            require(key not in result, f"duplicate JSON key in {name}: {key}")
            result[key] = value
        return result

    def constant(value: str) -> None:
        raise VerificationError(f"invalid JSON constant in {name}: {value}")

    try:
        result = json.loads(data.decode("utf-8"), object_pairs_hook=pairs,
                            parse_constant=constant)
    except (UnicodeError, ValueError, RecursionError) as error:
        raise VerificationError(f"invalid JSON {name}: {error}") from error
    require(isinstance(result, dict), f"{name} must be an object")
    return result


def exact_keys(value: object, keys: set, name: str) -> None:
    require(isinstance(value, dict) and set(value) == keys, f"invalid {name} fields")


def valid_hash(value: object, pattern=HEX64) -> bool:
    return isinstance(value, str) and pattern.fullmatch(value) is not None


def platforms(product: str) -> tuple[str, ...]:
    require(product in PROFILES, f"unknown product: {product}")
    return PROFILES[product]


def expected_assets(product: str, version: str) -> tuple[str, ...]:
    require(isinstance(version, str) and VERSION.fullmatch(version) is not None,
            "version must be strict X.Y.Z")
    return tuple(f"{product}-{version}-{platform}." +
                 ("zip" if platform == "windows-x86_64" else "tar.gz")
                 for platform in platforms(product)) + (
                     f"{product}-{version}-source.tar.gz", "SHA256SUMS")


def allowed_documents(product: str, tag: str) -> frozenset[str]:
    extra = {"HARDWARE-VALIDATION.md"} if product == "asicen-userland" else set()
    return DOCUMENTS | extra | {f"release-notes/{tag}.md", f"release-notes/{tag}.json"}


def safe_name(name: str) -> None:
    require(isinstance(name, str) and bool(name) and len(name) <= 1024,
            "invalid archive path length")
    require(not any(ord(c) < 32 or ord(c) == 127 for c in name) and
            "\\" not in name and ":" not in name and
            all(part not in {"", ".", ".."} for part in name.split("/")),
            f"unsafe archive path: {name!r}")


def validate_record(record: dict, *, product: str, tag: str,
                    repository: str | None = None) -> dict:
    exact_keys(record, {"schema", "product", "repository", "version", "notes_sha256",
                        "candidate", "reproduction", "gates"}, "release record")
    require(type(record["schema"]) is int and record["schema"] == 1, "record schema must be 1")
    version = record["version"]
    assets = set(expected_assets(product, version))
    require(record["product"] == product and tag == f"v{version}", "record product/tag mismatch")
    require(isinstance(record["repository"], str) and
            re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", record["repository"]),
            "invalid record repository")
    if repository is not None:
        require(record["repository"] == repository, "record repository mismatch")
    require(valid_hash(record["notes_sha256"]), "invalid notes SHA-256")
    for label in ("candidate", "reproduction"):
        run = record[label]
        exact_keys(run, {"run_id", "run_attempt", "artifact_id", "sha", "tree", "workflow",
                         "artifact_name", "assets_sha256", "build_inputs", "runner_images"}, label)
        for key in ("run_id", "run_attempt", "artifact_id"):
            require(type(run[key]) is int and run[key] > 0, f"invalid {label} {key}")
        require(valid_hash(run["sha"], HEX40) and valid_hash(run["tree"], HEX40),
                f"invalid {label} commit/tree")
        require(run["workflow"] == WORKFLOW and run["artifact_name"] == "release-candidate",
                f"invalid {label} workflow/artifact")
        exact_keys(run["assets_sha256"], assets, f"{label} asset hashes")
        require(all(valid_hash(v) for v in run["assets_sha256"].values()),
                f"invalid {label} asset hash")
        for key in ("build_inputs", "runner_images"):
            values = run[key]
            require(isinstance(values, dict) and bool(values) and
                    all(isinstance(k, str) and k.strip() and isinstance(v, str) and v.strip()
                        for k, v in values.items()), f"missing {label} {key}")
    a, b = record["candidate"], record["reproduction"]
    require(a["run_id"] != b["run_id"] and a["artifact_id"] != b["artifact_id"],
            "reproducibility requires distinct runs and artifacts")
    for key in ("sha", "tree", "assets_sha256", "build_inputs"):
        require(a[key] == b[key], f"reproducibility {key} mismatch (not a passing comparison)")
    gates = record["gates"]
    exact_keys(gates, {"hardware", "specification_tests", "soak_decision", "support_review",
                       "prepublication_review"}, "gates")
    exact_keys(gates["hardware"], set(platforms(product)), "hardware gates")
    documents = allowed_documents(product, tag) - {f"release-notes/{tag}.json"}
    for name, gate in list(gates["hardware"].items()) + [
            (key, value) for key, value in gates.items() if key != "hardware"]:
        exact_keys(gate, {"status", "evidence"}, f"gate {name}")
        require(gate["status"] == "completed", f"gate {name} is not completed")
        require(isinstance(gate["evidence"], list) and bool(gate["evidence"]),
                f"gate {name} lacks evidence")
        for evidence in gate["evidence"]:
            require(isinstance(evidence, str) and evidence.count("#") == 1,
                    f"gate {name} evidence needs a document and section")
            path, anchor = evidence.split("#")
            require(path in documents and bool(anchor.strip()) and
                    not any(c.isspace() for c in anchor), f"invalid gate {name} evidence")
    return record


@dataclass(frozen=True)
class Limits:
    archive_bytes: int = 1024 * 1024 * 1024
    member_bytes: int = 128 * 1024 * 1024
    total_bytes: int = 1024 * 1024 * 1024
    members: int = 50000
    metadata_bytes: int = 8 * 1024 * 1024


@dataclass(frozen=True)
class Member:
    mode: int
    size: int
    sha256: str
    data: bytes | None


def read_archive(path: Path, *, limits: Limits = Limits()) -> dict[str, Member]:
    """Hash regular members without extracting or executing any content.

    Existing package writers only emit regular files. Rejecting directories,
    symlinks, hardlinks, sparse files and devices is intentionally stricter than
    merely comparing their type/link target; no link can escape this boundary.
    """
    require(path.is_file() and not path.is_symlink(), f"not a regular archive: {path}")
    require(path.stat().st_size <= limits.archive_bytes, f"archive size limit: {path}")
    members = {}
    folded_names = set()
    total = 0

    def consume(name: str, mode: int, size: int, stream) -> None:
        nonlocal total
        safe_name(name)
        require(name not in members, f"duplicate archive member: {name}")
        require(name.casefold() not in folded_names, f"case-colliding archive member: {name}")
        folded_names.add(name.casefold())
        require(len(members) < limits.members, "archive member count limit")
        require(0 <= size <= limits.member_bytes, f"archive member size limit: {name}")
        total += size
        require(total <= limits.total_bytes, "archive expanded size limit")
        require(0 <= mode <= 0o777, f"unsupported archive mode for {name}: {mode:o}")
        # Retain only control files. Payload comparison uses their exact hashes.
        keep = (name in {"README.md", "SHA256SUMS", "manifest.json", "source-manifest.json",
                         "BUILD-RELINK.md", "repository/README.md", "repository/VERSION",
                         "repository/packaging/REBUILD.md.in"} or
                name in ASICEN_PROVENANCE or name.startswith("repository/release-notes/"))
        if keep:
            require(size <= limits.metadata_bytes, f"metadata size limit: {name}")
        value, chunks, seen = hashlib.sha256(), [], 0
        while True:
            chunk = stream.read(min(1024 * 1024, size - seen + 1))
            if not chunk:
                break
            seen += len(chunk)
            require(seen <= size, f"member length mismatch: {name}")
            value.update(chunk)
            if keep:
                chunks.append(chunk)
        require(seen == size, f"truncated archive member: {name}")
        members[name] = Member(mode, size, value.hexdigest(), b"".join(chunks) if keep else None)

    try:
        if path.name.endswith(".zip"):
            with zipfile.ZipFile(path) as archive:
                require(len(archive.infolist()) <= limits.members, "archive member count limit")
                for info in archive.infolist():
                    mode = info.external_attr >> 16
                    require(info.create_system == 3 and stat.S_ISREG(mode) and
                            not info.is_dir() and not info.flag_bits & 1 and
                            info.compress_type in {zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED},
                            f"unsupported ZIP member: {info.filename}")
                    with archive.open(info) as stream:
                        consume(info.filename, stat.S_IMODE(mode), info.file_size, stream)
        else:
            require(path.name.endswith(".tar.gz"), f"unsupported archive format: {path}")
            # Cap decompression, including headers and trailers that member-size
            # limits alone do not cover. tarfile never receives an unbounded
            # extension-header read. The wrapper is forward-only: no repeated
            # decompression or path-dependent seek cost.
            class BoundedReader:
                def __init__(self, stream):
                    self.stream, self.count = stream, 0

                def read(self, size=-1):
                    require(0 <= size <= limits.metadata_bytes, "tar parser read size limit")
                    value = self.stream.read(size)
                    self.count += len(value)
                    require(self.count <= limits.total_bytes + limits.members * 2048 + 20480,
                            "tar decompressed size limit")
                    return value

            with gzip.open(path, "rb") as raw:
                bounded = BoundedReader(raw)
                with tarfile.open(fileobj=bounded, mode="r|") as archive:
                    for info in archive:
                        require(info.isfile() and info.sparse is None and not
                                any(k.startswith("GNU.sparse") for k in info.pax_headers),
                                f"unsupported tar member: {info.name}")
                        stream = archive.extractfile(info)
                        require(stream is not None, f"unreadable tar member: {info.name}")
                        with stream:
                            consume(info.name, info.mode, info.size, stream)
                    # tarfile's stream buffers may already include the trailer.
                    # Only zero padding may follow the end-of-archive marker.
                    require(not archive.fileobj.buf.strip(b"\0"), "nonzero tar trailer")
                    while True:
                        trailer = bounded.read(min(1024 * 1024, limits.metadata_bytes))
                        if not trailer:
                            break
                        require(not trailer.strip(b"\0"), "nonzero tar trailer")
    except (OSError, tarfile.TarError, zipfile.BadZipFile, EOFError, RuntimeError) as error:
        raise VerificationError(f"cannot read archive {path}: {error}") from error
    require(bool(members), f"empty archive: {path}")
    for name in members:
        parts = name.split("/")
        require(not any("/".join(parts[:i]) in members for i in range(1, len(parts))),
                f"file/parent conflict: {name}")
    return members


def checksum_map(data: bytes, name: str) -> dict[str, str]:
    try:
        text = data.decode("ascii")
    except UnicodeError as error:
        raise VerificationError(f"non-ASCII checksums: {name}") from error
    require(text.endswith("\n") and "\r" not in text, f"invalid checksum lines: {name}")
    result = {}
    for line in text.splitlines():
        require(len(line) > 66 and valid_hash(line[:64]) and line[64:66] == "  ",
                f"invalid checksum entry in {name}")
        member = line[66:]
        safe_name(member)
        require(member not in result, f"duplicate checksum entry: {member}")
        result[member] = line[:64]
    return result


def content(members: dict[str, Member], name: str) -> bytes:
    require(name in members and members[name].data is not None, f"missing control file: {name}")
    return members[name].data


def check_manifest(members: dict[str, Member], name: str) -> dict:
    checksums = checksum_map(content(members, "SHA256SUMS"), "inner SHA256SUMS")
    require(checksums == {n: v.sha256 for n, v in members.items() if n != "SHA256SUMS"},
            "inner checksum inventory/hash mismatch")
    manifest = load_json(content(members, name), name)
    require(type(manifest.get("schema")) is int and manifest["schema"] == 1,
            "invalid archive manifest schema")
    files = manifest.get("files")
    require(isinstance(files, dict) and set(files) == set(members) - {name, "SHA256SUMS"},
            "manifest inventory mismatch")
    for filename, record in files.items():
        exact_keys(record, {"size", "sha256"}, f"manifest file {filename}")
        require(type(record["size"]) is int and record["size"] == members[filename].size and
                record["sha256"] == members[filename].sha256, f"manifest mismatch: {filename}")
    return manifest


def verify_bundle(directory: Path, product: str, version: str) -> dict[str, str]:
    require(directory.is_dir() and not directory.is_symlink(), f"not a bundle directory: {directory}")
    expected = set(expected_assets(product, version))
    require({p.name for p in directory.iterdir()} == expected, "release asset inventory mismatch")
    hashes = {name: file_digest(directory / name) for name in expected}
    require((directory / "SHA256SUMS").stat().st_size <= 16384, "outer checksums too large")
    listed = checksum_map((directory / "SHA256SUMS").read_bytes(), "outer SHA256SUMS")
    require(listed == {n: v for n, v in hashes.items() if n != "SHA256SUMS"},
            "outer checksum inventory/hash mismatch")
    return hashes


def compare_members(before: dict, after: dict, permitted: set[str] | frozenset[str]) -> list[str]:
    require(set(before) == set(after), "archive inventory changed (including documents)")
    changed = []
    for name in before:
        a, b = before[name], after[name]
        require(a.mode == b.mode, f"archive mode changed: {name}")
        if (a.size, a.sha256) != (b.size, b.sha256):
            require(name in permitted, f"payload changed: {name}")
            changed.append(name)
    return sorted(changed)


def compare_manifests(a: dict, b: dict, identity_fields: set[str], allowed_files: set[str]) -> None:
    require({k: v for k, v in a.items() if k not in identity_fields | {"files"}} ==
            {k: v for k, v in b.items() if k not in identity_fields | {"files"}},
            "undocumented manifest metadata changed")
    require(set(a["files"]) == set(b["files"]), "manifest file inventory changed")
    for name in a["files"]:
        require(name in allowed_files or a["files"][name] == b["files"][name],
                f"undocumented manifest file metadata changed: {name}")


def check_source(members: dict, version: str, sha: str, tree: str, product: str) -> dict:
    manifest = check_manifest(members, "source-manifest.json")
    require(manifest.get("version") == version and manifest.get("kind") == "corresponding-source" and
            manifest.get("repository_commit") == sha and manifest.get("repository_tree") == tree,
            "source manifest version/commit/tree mismatch")
    require(manifest.get("libusb_version") == "1.0.30" and
            manifest.get("libusb_archive_sha256") == LIBUSB_SHA256,
            "source manifest libusb identity mismatch")
    require(content(members, "repository/VERSION") == (version + "\n").encode(),
            "source VERSION mismatch")
    require(content(members, "README.md") == content(members, "repository/README.md"),
            "source README does not match repository snapshot")
    template = content(members, "repository/packaging/REBUILD.md.in")
    expected = template.replace(b"@VERSION@", version.encode()).replace(
        b"@COMMIT@", sha.encode()).replace(b"@TREE@", tree.encode())
    require(content(members, "BUILD-RELINK.md") == expected, "BUILD-RELINK does not match source template")
    for name in members:
        parts = name.lower().split("/")
        require("firmware" not in parts and not name.lower().endswith((".pyc", ".pyo", ".pyd")) and
                "__pycache__" not in parts and ".git" not in parts,
                f"forbidden corresponding-source material: {name}")
    return manifest


def check_binary(members: dict, *, product: str, platform: str, version: str,
                 sha: str, readme: bytes) -> dict:
    manifest = check_manifest(members, "manifest.json")
    require(manifest.get("version") == version and manifest.get("platform") == platform and
            manifest.get("source_ref") == sha, "binary manifest version/platform/source_ref mismatch")
    require(manifest.get("architecture") == platform.rsplit("-", 1)[-1],
            "binary manifest architecture mismatch")
    libc = ("glibc" if platform.startswith("linux-glibc-") else
            "musl" if platform.startswith("linux-musl-") else
            "android" if platform.startswith("android-") else
            "ucrt" if platform == "windows-x86_64" else "darwin")
    require(manifest.get("libc") == libc and manifest.get("production_only") is True and
            manifest.get("embedded_libusb") == {"version": "1.0.30", "linkage": "static"},
            "binary manifest runtime metadata mismatch")
    programs = ["px4d", "px4-ts", "px4ctl"] if product == "px4-userland" else [
        "asicend", "asicen-ts", "asicenctl"]
    require(manifest.get("programs") == programs, "binary manifest programs mismatch")
    for program in programs:
        filename = program + (".exe" if platform == "windows-x86_64" else "")
        require(filename in members and members[filename].mode == 0o755,
                f"missing executable: {filename}")
    require(content(members, "README.md") == readme, "binary README does not match source snapshot")
    if product == "asicen-userland":
        require(FIRMWARE_MEMBER in members and members[FIRMWARE_MEMBER].sha256 == FIRMWARE_SHA256 and
                members[FIRMWARE_MEMBER].size == 16384 and members[FIRMWARE_MEMBER].mode == 0o644,
                "ASICEN firmware differs from pinned loader")
        require("licenses/VENDOR-FIRMWARE-NOTICE.txt" in members, "missing ASICEN firmware notice")
    else:
        require(not any("firmware" in name.lower().split("/") for name in members),
                "PX4 binary contains forbidden firmware")
    return manifest


def check_asicen_provenance(members: dict, sha: str, platform: str) -> dict[str, dict]:
    result = {}
    for name in ASICEN_PROVENANCE:
        metadata = load_json(content(members, name), name)
        require(metadata.get("source_ref") == sha, f"ASICEN provenance source_ref mismatch: {name}")
        if name.endswith("linux-static-build.json"):
            exact_keys(metadata, {"schema", "source_ref", "compiler_target", "libc_archive_sha256",
                                  "libc_identification", "production_binary_sha256"}, name)
            hashes = {n: members[n].sha256 for n in ("asicend", "asicen-ts", "asicenctl")}
            require(metadata["production_binary_sha256"] == hashes and
                    valid_hash(metadata["libc_archive_sha256"]), "ASICEN static provenance hashes mismatch")
            libc = "musl"
        else:
            exact_keys(metadata, {"schema", "source_ref", "compiler_target", "library_sha256"}, name)
            library = "ifd/asicen-userland-ifd.so"
            require(library in members and metadata["library_sha256"] == members[library].sha256,
                    "ASICEN IFD provenance hash mismatch")
            libc = "musl" if platform.startswith("linux-musl-") else "gnu"
        require(type(metadata["schema"]) is int and metadata["schema"] == 1 and
                isinstance(metadata["compiler_target"], str) and
                re.fullmatch(re.escape(platform.rsplit("-", 1)[-1]) +
                             r"-[A-Za-z0-9_.+-]*linux-" + libc + r"[A-Za-z0-9_.+-]*",
                             metadata["compiler_target"]), "ASICEN provenance compiler mismatch")
        result[name] = metadata
    return result


def verify_release(record_path: Path, candidate_dir: Path, reproduction_dir: Path, tag_dir: Path,
                   *, product: str, tag: str, tag_sha: str, tag_tree: str,
                   repository: str | None = None) -> dict:
    require(valid_hash(tag_sha, HEX40) and valid_hash(tag_tree, HEX40), "invalid tag commit/tree")
    record_path = Path(record_path)
    require(record_path.is_file() and not record_path.is_symlink() and
            record_path.stat().st_size <= Limits().metadata_bytes, "invalid release record file")
    raw_record = record_path.read_bytes()
    record = validate_record(load_json(raw_record, "release record"), product=product,
                             tag=tag, repository=repository)
    candidate_dir, reproduction_dir, tag_dir = map(Path, (candidate_dir, reproduction_dir, tag_dir))
    version, candidate = record["version"], record["candidate"]
    candidate_hashes = verify_bundle(candidate_dir, product, version)
    reproduction_hashes = verify_bundle(reproduction_dir, product, version)
    tag_hashes = verify_bundle(tag_dir, product, version)
    require(candidate_hashes == candidate["assets_sha256"], "candidate bytes differ from pinned record")
    require(reproduction_hashes == record["reproduction"]["assets_sha256"],
            "reproduction bytes differ from pinned record")
    # In addition to digests, compare unmodified archive/checksum bytes directly.
    for name in expected_assets(product, version):
        with (candidate_dir / name).open("rb") as a, (reproduction_dir / name).open("rb") as b:
            while True:
                left, right = a.read(1024 * 1024), b.read(1024 * 1024)
                require(left == right, f"reproducibility byte mismatch: {name}")
                if not left:
                    break
    source_name = f"{product}-{version}-source.tar.gz"
    before_source = read_archive(candidate_dir / source_name)
    after_source = read_archive(tag_dir / source_name)
    before_manifest = check_source(before_source, version, candidate["sha"], candidate["tree"], product)
    after_manifest = check_source(after_source, version, tag_sha, tag_tree, product)
    docs = {"repository/" + path for path in allowed_documents(product, tag)}
    source_allowed = docs | {"README.md", "BUILD-RELINK.md"}
    changes = {source_name: compare_members(before_source, after_source,
                                            source_allowed | {"source-manifest.json", "SHA256SUMS"})}
    compare_manifests(before_manifest, after_manifest, {"repository_commit", "repository_tree"},
                      source_allowed)
    require(content(after_source, f"repository/release-notes/{tag}.json") == raw_record,
            "release record differs from tag source snapshot")
    notes = content(after_source, f"repository/release-notes/{tag}.md")
    require(digest(notes) == record["notes_sha256"] and notes.startswith(
        f"# {product} {tag}\n".encode()), "release notes hash/heading mismatch")
    all_gates = list(record["gates"]["hardware"].values()) + [
        value for key, value in record["gates"].items() if key != "hardware"]
    for gate in all_gates:
        for evidence in gate["evidence"]:
            path = "repository/" + evidence.split("#")[0]
            require(path in after_source and after_source[path].size > 0,
                    f"gate evidence document missing: {path}")
    for platform in platforms(product):
        extension = "zip" if platform == "windows-x86_64" else "tar.gz"
        name = f"{product}-{version}-{platform}.{extension}"
        before, after = read_archive(candidate_dir / name), read_archive(tag_dir / name)
        a = check_binary(before, product=product, platform=platform, version=version,
                         sha=candidate["sha"], readme=content(before_source, "README.md"))
        b = check_binary(after, product=product, platform=platform, version=version,
                         sha=tag_sha, readme=content(after_source, "README.md"))
        allowed = {"README.md"}
        if product == "asicen-userland" and platform.startswith("linux-"):
            old = check_asicen_provenance(before, candidate["sha"], platform)
            new = check_asicen_provenance(after, tag_sha, platform)
            for filename in ASICEN_PROVENANCE:
                require({k: v for k, v in old[filename].items() if k != "source_ref"} ==
                        {k: v for k, v in new[filename].items() if k != "source_ref"},
                        f"ASICEN build provenance changed: {filename}")
            allowed |= ASICEN_PROVENANCE
        changes[name] = compare_members(before, after, allowed | {"manifest.json", "SHA256SUMS"})
        compare_manifests(a, b, {"source_ref"}, allowed)
    return {"schema": 1, "product": product, "version": version, "tag": tag,
            "tag_sha": tag_sha, "tag_tree": tag_tree, "candidate_run_id": candidate["run_id"],
            "reproduction_run_id": record["reproduction"]["run_id"],
            "candidate_sha": candidate["sha"], "record_sha256": digest(raw_record),
            "notes_sha256": digest(notes), "assets_sha256": tag_hashes,
            "allowed_changes": changes, "verified": True}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("verify", choices=["verify"])
    for flag in ("record", "candidate-dir", "reproduction-dir", "tag-dir"):
        parser.add_argument("--" + flag, type=Path, required=True)
    for flag in ("product", "tag", "tag-sha", "tag-tree"):
        parser.add_argument("--" + flag, required=True)
    parser.add_argument("--repository")
    args = parser.parse_args()
    try:
        result = verify_release(args.record, args.candidate_dir, args.reproduction_dir, args.tag_dir,
                                product=args.product, tag=args.tag, tag_sha=args.tag_sha,
                                tag_tree=args.tag_tree, repository=args.repository)
    except (VerificationError, OSError) as error:
        print(f"release verification failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
