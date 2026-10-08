#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Audit the firmware-free macOS arm64 intermediate candidate."""
import argparse
import ctypes
import gzip
import hashlib
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
COMMANDS = ("asicend", "asicen-ts", "asicenctl")
IFD_EXPORTS = (
    "IFDHCreateChannel", "IFDHCreateChannelByName", "IFDHCloseChannel",
    "IFDHGetCapabilities", "IFDHSetCapabilities", "IFDHSetProtocolParameters",
    "IFDHPowerICC", "IFDHTransmitToICC", "IFDHControl", "IFDHICCPresence",
)
LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
FIRMWARE_SHA256 = "b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8"
BASE_FILES = {
    *(f"bin/{name}" for name in COMMANDS),
    "pcsc/ASICEN-IFD.bundle/Contents/Info.plist",
    "pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib",
    "source/asicen-userland-source.tar.gz", "source/libusb-1.0.30.tar.bz2",
    "source/asicen-macos-corresponding-source.tar.gz",
    "licenses/COPYING.gpl2", "licenses/px4-userland-GPL-2.0-only.txt",
    "licenses/libusb-COPYING", "licenses/NOTICES.md",
    "BUILD-METADATA.txt", "SOURCE-MANIFEST.txt", "REBUILD.md", "SHA256SUMS",
}
FINAL_FILES = {"firmware/asicen-loader.bin", "licenses/VENDOR-FIRMWARE-NOTICE.txt"}


def fail(message: str) -> None:
    raise ValueError(message)


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def command(*args: str) -> str:
    result = subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE)
    return result.stdout


def audit_macho(path: Path, expected_install_id: str | None = None) -> None:
    architectures = command("lipo", "-archs", str(path)).split()
    if architectures != ["arm64"]:
        fail(f"{path.name} is not a single-architecture arm64 Mach-O: {architectures}")
    linkage = command("otool", "-L", str(path)).splitlines()[1:]
    install_lines = command("otool", "-D", str(path)).splitlines()[1:]
    install_ids = [line.strip() for line in install_lines if line.strip()]
    if expected_install_id is None:
        if install_ids:
            fail(f"unexpected Mach-O install ID in {path.name}: {install_ids}")
    elif install_ids != [expected_install_id]:
        fail(f"unexpected Mach-O install ID in {path.name}: {install_ids}")
    for line in linkage:
        dependency = line.strip().split(" ", 1)[0]
        if expected_install_id is not None and dependency == expected_install_id:
            continue
        if not dependency.startswith(("/System/Library/", "/usr/lib/")):
            fail(f"non-system Mach-O dependency in {path.name}: {dependency}")
    load_commands = command("otool", "-l", str(path))
    if re.search(r"\bLC_RPATH\b", load_commands):
        fail(f"LC_RPATH is forbidden in {path.name}")
    if any("libusb" in line.lower() for line in linkage):
        fail(f"dynamic libusb dependency in {path.name}")


def parse_sums(path: Path) -> dict[str, str]:
    sums: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([A-Za-z0-9_./+-]+)", line)
        if not match:
            fail(f"malformed SHA256SUMS entry: {line!r}")
        relative = PurePosixPath(match.group(2))
        if relative.is_absolute() or ".." in relative.parts or str(relative) in sums:
            fail(f"unsafe or duplicate checksum path: {relative}")
        sums[str(relative)] = match.group(1)
    return sums


def verify_bundle(bundle: Path) -> None:
    executable = bundle / "Contents/MacOS/libifd-asicen.dylib"
    plist_path = bundle / "Contents/Info.plist"
    if not executable.is_file() or not plist_path.is_file():
        fail("IFD bundle is incomplete")
    with plist_path.open("rb") as stream:
        info = plistlib.load(stream)
    if info.get("CFBundleIdentifier") != "io.github.khronos31.asicen-userland.ifd":
        fail("IFD bundle identifier mismatch")
    if info.get("CFBundleExecutable") != executable.name:
        fail("IFD bundle executable does not match the bundled library")
    audit_macho(executable, "@rpath/libifd-asicen.dylib")
    symbols = command("nm", "-gU", str(executable))
    for symbol in IFD_EXPORTS:
        if not re.search(rf"\b_{re.escape(symbol)}\s*$", symbols, re.MULTILINE):
            fail(f"IFD export is missing: {symbol}")
    # Load the extracted native plugin without registering it with system PC/SC.
    loaded = ctypes.CDLL(str(executable), mode=getattr(os, "RTLD_NOW", 2))
    for symbol in IFD_EXPORTS:
        getattr(loaded, symbol)


def safe_inventory(root: Path) -> set[str]:
    found = set()
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"candidate contains symlink: {path.relative_to(root)}")
        if path.is_file():
            found.add(path.relative_to(root).as_posix())
    return found


def verify_source_archives(root: Path, source_sha: str) -> None:
    source = root / "source/asicen-userland-source.tar.gz"
    libusb = root / "source/libusb-1.0.30.tar.bz2"
    if digest(source) != source_sha:
        fail("source archive SHA-256 differs from build metadata")
    if digest(libusb) != LIBUSB_SHA256:
        fail("candidate does not carry the pinned libusb 1.0.30 archive")
    with tarfile.open(source, "r:gz") as archive:
        names = sorted(member.name for member in archive.getmembers())
        for member in archive.getmembers():
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts or name.parts[:1] in (("firmware",), ("out",)):
                fail(f"unsafe or excluded project source path: {member.name}")
            if "asicen-loader.bin" in name.parts or member.issym() or member.islnk():
                fail(f"source archive contains forbidden payload/link: {member.name}")
    manifest = (root / "SOURCE-MANIFEST.txt").read_text(encoding="utf-8").splitlines()
    if manifest != names:
        fail("source manifest does not match the immutable source archive")
    with tarfile.open(root / "source/asicen-macos-corresponding-source.tar.gz", "r:gz") as archive:
        members = {member.name: member for member in archive.getmembers() if member.isfile()}
        expected = {
            "source/asicen-userland-source.tar.gz", "source/libusb-1.0.30.tar.bz2",
            "SOURCE-MANIFEST.txt", "REBUILD.md", "licenses/COPYING.gpl2",
            "licenses/px4-userland-GPL-2.0-only.txt", "licenses/libusb-COPYING",
            "licenses/NOTICES.md",
        }
        if set(members) != expected:
            fail("corresponding-source bundle inventory mismatch")
        for relative in expected:
            stream = archive.extractfile(members[relative])
            if stream is None:
                fail(f"corresponding-source member unavailable: {relative}")
            bundled_hash = hashlib.sha256(stream.read()).hexdigest()
            if bundled_hash != digest(root / relative):
                fail(f"corresponding-source member does not match candidate: {relative}")


def audit_candidate(root: Path, final: bool = False, run_smoke: bool = True) -> None:
    if root.is_symlink() or not root.is_dir():
        fail("candidate must be a real directory")
    expected = BASE_FILES | (FINAL_FILES if final else set())
    actual = safe_inventory(root)
    if actual != expected:
        fail(f"candidate inventory mismatch: missing={sorted(expected-actual)}, extra={sorted(actual-expected)}")
    executable_paths = {f"bin/{name}" for name in COMMANDS} | {
        "pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib"}
    for relative in actual:
        mode = (root / relative).stat().st_mode & 0o777
        required_mode = 0o755 if relative in executable_paths else 0o644
        if mode != required_mode:
            fail(f"unexpected candidate file mode {mode:o}: {relative}")
    metadata = (root / "BUILD-METADATA.txt").read_text(encoding="utf-8")
    expected_class = "final macOS arm64 package" if final else "intermediate macOS arm64 candidate"
    if f"Candidate class: {expected_class}" not in metadata:
        fail("candidate class metadata mismatch")
    if "macOS deployment target: 14.0" not in metadata:
        fail("candidate deployment target metadata mismatch")
    source_match = re.search(r"^Source archive SHA-256: ([0-9a-f]{64})$", metadata, re.MULTILINE)
    if not source_match:
        fail("source archive hash missing from metadata")
    verify_source_archives(root, source_match.group(1))
    sums = parse_sums(root / "SHA256SUMS")
    actual_sums = {name: digest(root / name) for name in actual if name != "SHA256SUMS"}
    if sums != actual_sums:
        fail("SHA256SUMS does not cover exactly the candidate files")
    for command_name in COMMANDS:
        binary = root / "bin" / command_name
        audit_macho(binary)
        if subprocess.run([str(binary), "--help"], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode != 0:
            fail(f"command help smoke failed: {command_name}")
    verify_bundle(root / "pcsc/ASICEN-IFD.bundle")
    if final:
        if "Firmware: included from external input; vendor redistribution rights unresolved" not in metadata:
            fail("final firmware metadata does not describe the external input")
        firmware = root / "firmware/asicen-loader.bin"
        notice = root / "licenses/VENDOR-FIRMWARE-NOTICE.txt"
        if firmware.stat().st_size != 16384 or digest(firmware) != FIRMWARE_SHA256:
            fail("final firmware does not match the recorded component")
        notice_text = notice.read_text(encoding="utf-8")
        if FIRMWARE_SHA256 not in notice_text or "Redistribution rights: unresolved" not in notice_text:
            fail("vendor firmware notice does not preserve the rights status")
    if run_smoke:
        script = root / "source/asicen-userland-source.tar.gz"
        with tempfile.TemporaryDirectory(prefix="asicen-macos-smoke-") as temporary:
            extracted = Path(temporary) / "source"
            extracted.mkdir()
            with tarfile.open(script, "r:gz") as archive:
                archive.extractall(extracted, filter="data")
            test_script = extracted / "userland/tests/product_cli_integration.py"
            subprocess.run([sys.executable, str(test_script),
                            str(root / "bin/asicend"), str(root / "bin/asicenctl"),
                            str(root / "bin/asicen-ts")], check=True, timeout=60,
                           env={**os.environ, "TMPDIR": "/tmp"})
            if sys.platform == "darwin":
                ifd_smoke = extracted / "scripts/macos-ifd-bundle-smoke.py"
                subprocess.run([sys.executable, str(ifd_smoke),
                                str(root / "pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib"),
                                str(root / "bin/asicend")], check=True, timeout=15,
                               env={**os.environ, "TMPDIR": "/tmp"})


def audit_candidate_archive(path: Path, final: bool = False) -> None:
    if path.is_symlink() or not path.is_file():
        fail("candidate archive must be a regular non-symlink file")
    with tempfile.TemporaryDirectory(prefix="asicen-macos-archive-") as temporary:
        root = Path(temporary) / "candidate"
        root.mkdir()
        with tarfile.open(path, "r:gz") as archive:
            members = archive.getmembers()
            for member in members:
                name = PurePosixPath(member.name)
                if name.is_absolute() or ".." in name.parts:
                    fail(f"unsafe path in candidate archive: {member.name}")
                if member.issym() or member.islnk() or not (member.isfile() or member.isdir()):
                    fail(f"unsupported member in candidate archive: {member.name}")
            archive.extractall(root, filter="data")
        extracted = root
        if (root / "bin").is_dir():
            extracted = root
        elif (root / "." / "bin").is_dir():
            extracted = root / "."
        else:
            fail("candidate archive does not contain the expected root layout")
        audit_candidate(extracted, final=final, run_smoke=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--final", action="store_true")
    parser.add_argument("--archive", type=Path,
                        help="audit an extracted candidate archive instead of a directory")
    args = parser.parse_args()
    try:
        if args.archive:
            audit_candidate_archive(args.archive.resolve(strict=True), final=args.final)
        else:
            audit_candidate(args.candidate.resolve(strict=True), final=args.final)
        print(f"macOS candidate audit: PASS ({'final' if args.final else 'intermediate'})")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(f"macOS candidate audit: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
