#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit a private static Linux candidate directory without running hardware."""
import argparse
import hashlib
import io
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile

FIRMWARE_SHA256 = "b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8"
LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
COMMANDS = ("asicend", "asicen-ts", "asicenctl")
LICENSES = (
    "licenses/COPYING.gpl2",
    "licenses/px4-userland-GPL-2.0-only.txt",
    "licenses/libusb-COPYING",
    "licenses/NOTICES.md",
    "licenses/musl-COPYRIGHT.txt",
    "licenses/GCC-COPYING3.txt",
    "licenses/GCC-COPYING.RUNTIME.txt",
)


def fail(message: str) -> None:
    raise ValueError(message)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def run_readelf(option: str, path: Path) -> str:
    result = subprocess.run(["readelf", option, str(path)], check=True,
                            text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    return result.stdout


def verify_static_elf(path: Path, expected_machine: str) -> None:
    header = run_readelf("-h", path)
    machine = re.search(r"^\s*Machine:\s*(.+)$", header, re.MULTILINE)
    if not machine or machine.group(1).strip() != expected_machine:
        fail(f"wrong ELF machine for {path.name}: {machine.group(1) if machine else 'unknown'}")
    program_headers = run_readelf("-l", path)
    if "INTERP" in program_headers:
        fail(f"static command has PT_INTERP: {path.name}")
    dynamic = run_readelf("-d", path)
    if "NEEDED" in dynamic:
        fail(f"static command has DT_NEEDED: {path.name}")
    versions = run_readelf("-V", path)
    if re.search(r"GLIBC_[0-9]", versions):
        fail(f"static command has GLIBC version requirements: {path.name}")


def parse_sums(path: Path) -> dict[str, str]:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_./+-]+)", line)
        if not match:
            fail(f"malformed SHA256SUMS line: {line!r}")
        relative = PurePosixPath(match.group(2))
        if relative.is_absolute() or ".." in relative.parts:
            fail(f"unsafe checksum path: {relative}")
        if str(relative) in result:
            fail(f"duplicate checksum path: {relative}")
        result[str(relative)] = match.group(1)
    return result


def audit_source_archive(path: Path) -> None:
    try:
        with tarfile.open(path, "r:gz") as archive:
            for member in archive.getmembers():
                name = PurePosixPath(member.name)
                if name.is_absolute() or ".." in name.parts:
                    fail(f"unsafe path in source archive: {member.name}")
                if name.parts and name.parts[0] in {"firmware", ".git", "build", "out"}:
                    fail(f"excluded material present in corresponding source: {member.name}")
                if member.issym() or member.islnk():
                    target = PurePosixPath(member.linkname)
                    if target.is_absolute() or ".." in target.parts:
                        fail(f"unsafe link in source archive: {member.name}")
                elif not (member.isfile() or member.isdir()):
                    fail(f"unsupported special file in source archive: {member.name}")
                if "asicen-loader.bin" in name.parts:
                    fail("vendor firmware leaked into corresponding-source archive")
    except (tarfile.TarError, OSError) as exc:
        fail(f"cannot read source archive: {exc}")


def audit_corresponding_source_bundle(bundle_path: Path,
                                     candidate_root: Path) -> None:
    expected_paths = {
        "source/asicen-userland-source.tar.gz",
        "source/libusb-1.0.30.tar.bz2",
        "SOURCE-MANIFEST.txt",
        "REBUILD.md",
    }
    for item in (candidate_root / "licenses").rglob("*"):
        if item.is_symlink():
            fail(f"symlink in candidate license material: {item.relative_to(candidate_root)}")
        if item.is_file():
            expected_paths.add(item.relative_to(candidate_root).as_posix())
    expected_names = expected_paths
    try:
        with tarfile.open(bundle_path, "r:gz") as archive:
            members = archive.getmembers()
            actual_names = set()
            for member in members:
                name = PurePosixPath(member.name)
                if name.is_absolute() or ".." in name.parts:
                    fail(f"unsafe path in corresponding-source bundle: {member.name}")
                if member.isdir():
                    continue
                if member.issym() or member.islnk() or not member.isfile():
                    fail(f"source bundle accepts only regular files and directories: {member.name}")
                actual_names.add(member.name)
            if actual_names != expected_names:
                fail(f"source bundle inventory mismatch: {sorted(actual_names)}")
            file_hashes = {}
            for member in members:
                if not member.isfile():
                    continue
                stream = archive.extractfile(member)
                if stream is None:
                    fail(f"source bundle file unavailable: {member.name}")
                h = hashlib.sha256()
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    h.update(block)
                file_hashes[member.name] = h.hexdigest()
    except (tarfile.TarError, OSError) as exc:
        fail(f"cannot read corresponding-source bundle: {exc}")
    expected_hashes = {rel: digest(candidate_root / rel) for rel in expected_paths}
    if file_hashes != expected_hashes:
        fail("corresponding-source bundle does not match the candidate source and relink materials")


def verify_link_map(map_path: Path, library_path: Path, working_directory: Path) -> None:
    expected = library_path.resolve(strict=True)
    if not map_path.is_file() or map_path.is_symlink():
        fail("link map must be a regular non-symlink file")
    candidates = set()
    for line in map_path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = re.match(r"\s*LOAD\s+(\S+)", line)
        if not match:
            continue
        candidate = Path(match.group(1))
        if not candidate.is_absolute():
            candidate = working_directory / candidate
        candidates.add(candidate.resolve(strict=False))
    if expected not in candidates:
        fail(f"link map does not show compiler-selected libc archive: {expected}")


def normalize_candidate_modes(root: Path) -> None:
    executable_paths = {"bin/asicend", "bin/asicen-ts", "bin/asicenctl",
                        "lib/pcsc/ifd-asicen.so"}
    root.chmod(0o755)
    for item in root.rglob("*"):
        if item.is_dir():
            item.chmod(0o755)
        elif item.is_file():
            mode = 0o755 if item.relative_to(root).as_posix() in executable_paths else 0o644
            item.chmod(mode)


def audit_candidate_modes(root: Path, include_ifd: bool = False) -> None:
    if (root.stat().st_mode & 0o777) != 0o755:
        fail("candidate root directory mode must be 0755")
    executable_paths = {"bin/asicend", "bin/asicen-ts", "bin/asicenctl"}
    if include_ifd:
        executable_paths.add("lib/pcsc/ifd-asicen.so")
    for item in root.rglob("*"):
        mode = item.stat().st_mode & 0o777
        expected = 0o755 if item.is_dir() or item.relative_to(root).as_posix() in executable_paths else 0o644
        if mode != expected:
            fail(f"candidate mode mismatch: {item.relative_to(root)} is {mode:04o}, expected {expected:04o}")


def audit_candidate(candidate: Path) -> dict[str, str]:
    if candidate.is_symlink() or not candidate.is_dir():
        fail("candidate must be a real directory")
    root = candidate.resolve(strict=True)
    metadata_path = root / "BUILD-METADATA.txt"
    sums_path = root / "SHA256SUMS"
    if metadata_path.is_symlink() or sums_path.is_symlink():
        fail("candidate metadata must not be symlinks")
    metadata = metadata_path.read_text(encoding="utf-8")
    if "intermediate Linux static command build; incomplete" not in metadata:
        fail("directory is not a recognized intermediate command candidate")
    target_match = re.search(r"^Compiler target: (.+)$", metadata, re.MULTILINE)
    libc_match = re.search(r"^Musl libc loader identification: (.+)$", metadata, re.MULTILINE)
    if not target_match or "linux-musl" not in target_match.group(1):
        fail("candidate metadata does not prove a musl compiler target")
    if not libc_match or "musl libc" not in libc_match.group(1).lower():
        fail("candidate metadata does not identify the compiler-selected musl loader")
    for field in ("GCC executable SHA-256", "G++ executable SHA-256",
                  "Musl libc archive SHA-256", "Musl libc loader SHA-256",
                  "Musl license source SHA-256", "GCC COPYING3 source SHA-256",
                  "GCC runtime exception source SHA-256"):
        if not re.search(rf"^{re.escape(field)}: [0-9a-f]{{64}}$", metadata, re.MULTILINE):
            fail(f"candidate metadata is missing toolchain provenance hash: {field}")
    arch_match = re.search(r"^Architecture: (x86_64|aarch64)$", metadata, re.MULTILINE)
    if not arch_match:
        fail("candidate metadata has unsupported or missing architecture")
    machine = {"x86_64": "Advanced Micro Devices X86-64",
               "aarch64": "AArch64"}[arch_match.group(1)]
    expected = {f"bin/{name}" for name in COMMANDS}
    expected.update({"source/asicen-userland-source.tar.gz",
                     "source/libusb-1.0.30.tar.bz2",
                     "source/asicen-linux-corresponding-source.tar.gz", "SOURCE-MANIFEST.txt",
                     "REBUILD.md", "BUILD-METADATA.txt"})
    expected.update(LICENSES)
    sums = parse_sums(sums_path)
    if set(sums) != expected:
        fail(f"SHA256SUMS inventory mismatch: missing={sorted(expected-set(sums))} extra={sorted(set(sums)-expected)}")
    actual_files = set()
    for item in root.rglob("*"):
        if item.is_symlink():
            fail(f"candidate contains a symlink: {item.relative_to(root)}")
        if item.is_file():
            actual_files.add(item.relative_to(root).as_posix())
    if actual_files != expected | {"SHA256SUMS"}:
        fail(f"candidate directory inventory mismatch: missing={sorted((expected | {'SHA256SUMS'})-actual_files)} extra={sorted(actual_files-(expected | {'SHA256SUMS'}))}")
    audit_candidate_modes(root)
    for relpath, expected_digest in sums.items():
        path = root / relpath
        if path.is_symlink() or not path.is_file():
            fail(f"candidate item must be a regular file: {relpath}")
        if digest(path) != expected_digest:
            fail(f"checksum mismatch: {relpath}")
    if digest(root / "source/libusb-1.0.30.tar.bz2") != LIBUSB_SHA256:
        fail("libusb source archive provenance mismatch")
    source_hash_match = re.search(r"^Source archive SHA-256: ([0-9a-f]{64})$",
                                  metadata, re.MULTILINE)
    if not source_hash_match or digest(root / "source/asicen-userland-source.tar.gz") != source_hash_match.group(1):
        fail("corresponding-source archive does not match build metadata")
    for relpath in LICENSES:
        if not (root / relpath).is_file():
            fail(f"required license/notice missing: {relpath}")
    audit_source_archive(root / "source/asicen-userland-source.tar.gz")
    audit_corresponding_source_bundle(
        root / "source/asicen-linux-corresponding-source.tar.gz", root)
    with tarfile.open(root / "source/asicen-userland-source.tar.gz", "r:gz") as archive:
        actual_manifest = sorted(archive.getnames())
    expected_manifest = (root / "SOURCE-MANIFEST.txt").read_text(encoding="utf-8").splitlines()
    if actual_manifest != expected_manifest:
        fail("source archive manifest does not match its contents")
    for name in COMMANDS:
        command = root / "bin" / name
        verify_static_elf(command, machine)
        subprocess.run([str(command), "--help"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    return {"architecture": arch_match.group(1), "libc": "musl",
            "commands": ",".join(COMMANDS), "status": "intermediate-command-candidate-passed"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", nargs="?", type=Path)
    parser.add_argument("--source-archive", type=Path)
    parser.add_argument("--verify-link-map", type=Path)
    parser.add_argument("--library", type=Path)
    parser.add_argument("--base", type=Path)
    parser.add_argument("--normalize-modes", type=Path)
    args = parser.parse_args()
    try:
        if args.source_archive is not None:
            audit_source_archive(args.source_archive)
            result = {"source_archive": args.source_archive.name, "status": "passed"}
        elif args.verify_link_map is not None:
            if args.library is None or args.base is None:
                parser.error("--verify-link-map requires --library and --base")
            verify_link_map(args.verify_link_map, args.library, args.base)
            result = {"link_map": args.verify_link_map.name, "status": "passed"}
        elif args.normalize_modes is not None:
            normalize_candidate_modes(args.normalize_modes)
            result = {"candidate": args.normalize_modes.name, "status": "normalized"}
        elif args.candidate is not None:
            result = audit_candidate(args.candidate)
        else:
            parser.error("provide candidate directory or --source-archive")
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print(f"audit failed: {exc}", file=sys.stderr)
        return 1
    for key, value in result.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
