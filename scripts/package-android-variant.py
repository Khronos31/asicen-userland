#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Assemble and verify one ABI-specific Android command archive."""
import argparse
import gzip
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VERSION = (ROOT / "VERSION").read_text(encoding="ascii").strip()
COMMANDS = ("asicend", "asicenctl", "asicen-ts")
ABI_INFO = {
    "aarch64": "/system/bin/linker64",
    "armv7a": "/system/bin/linker",
    "x86_64": "/system/bin/linker64",
}


def fail(message: str) -> None:
    raise ValueError(message)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def validate_inputs(commands: Path, libusb_archive: Path, ndk: Path, abi: str) -> None:
    if commands.is_symlink() or not commands.is_dir():
        fail("command directory must be a real directory")
    for name in COMMANDS:
        path = commands / f"{name}-{abi}"
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            fail(f"missing ABI command: {path.name}")
    if libusb_archive.is_symlink() or not libusb_archive.is_file():
        fail("libusb archive must be a regular non-symlink file")
    for required in (ndk / "source.properties", ndk / "NOTICE"):
        if required.is_symlink() or not required.is_file():
            fail(f"NDK release material is missing: {required}")
    for required in (ROOT / "COPYING.gpl2", ROOT / "NOTICES.md", ROOT / "README.md",
                     ROOT / "packaging/termux/asicen-termux"):
        if required.is_symlink() or not required.is_file():
            fail(f"required repository file is missing: {required}")


def libusb_copying(libusb_archive: Path) -> bytes:
    with tarfile.open(libusb_archive, "r:bz2") as archive:
        member = archive.getmember("libusb-1.0.30/COPYING")
        stream = archive.extractfile(member)
        if stream is None:
            fail("libusb COPYING is not a regular file")
        return stream.read()


def rebuild_instructions(abi: str) -> str:
    return (
        "# Rebuild this Android command archive\n\n"
        f"These commands were built for Android API 24 with a static libusb 1.0.30\n"
        f"and the toolchain's static libc++, targeting ABI {abi}. No firmware is\n"
        "included. Requires the Android NDK r27, a C/C++ compiler with autotools\n"
        "for the host, CMake, Ninja, GNU make, curl, and Python 3.\n\n"
        "    export ANDROID_NDK_HOME=/path/to/android-ndk-r27\n"
        "    sh scripts/build-android.sh --abi " + abi + " --output ../android-" + abi + "\n\n"
        "The static libusb input is either downloaded (SHA-256\n"
        "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf) or supplied\n"
        "with --libusb-archive. Each command must then pass\n"
        "scripts/verify-android-elf.sh with its interpreter and saved architecture.\n"
        "Firmware is a separate vendor input and is never part of this archive.\n"
    )


def write_reproducible_archive(root: Path, archive_path: Path) -> None:
    raw_tar = Path(str(archive_path) + ".rawtar")
    with tarfile.open(raw_tar, "w") as tar:
        for item in sorted(root.rglob("*")):
            info = tar.gettarinfo(str(item), arcname=item.relative_to(root).as_posix())
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if item.is_file():
                with item.open("rb") as stream:
                    tar.addfile(info, stream)
            else:
                tar.addfile(info)
    with raw_tar.open("rb") as source, archive_path.open("wb") as destination:
        with gzip.GzipFile(filename="", mode="wb", fileobj=destination,
                           compresslevel=9, mtime=0) as compressor:
            shutil.copyfileobj(source, compressor)
    raw_tar.unlink()


def verify_archive(archive_path: Path, abi: str) -> None:
    verifier = ROOT / "scripts/verify-android-elf.sh"
    if verifier.is_symlink() or not verifier.is_file():
        fail("scripts/verify-android-elf.sh is missing")
    if archive_path.stat().st_size == 0:
        fail("archive is empty")
    with tempfile.TemporaryDirectory(prefix="asicen-android-verify-") as temporary:
        with tarfile.open(archive_path, "r:gz") as archive:
            archive.extractall(temporary, filter="data")
        unpacked = Path(temporary)
        for name in COMMANDS:
            binary = unpacked / "bin" / name
            if binary.is_symlink() or not binary.is_file():
                fail(f"archive does not contain bin/{name}")
            result = subprocess.run(
                [str(verifier), str(binary), ABI_INFO[abi], abi],
                text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if result.returncode != 0:
                fail(f"verify-android-elf.sh rejected bin/{name}: {result.stderr.strip()}")


def assemble(commands: Path, libusb_archive: Path, ndk: Path, output_dir: Path,
             abi: str) -> Path:
    validate_inputs(commands, libusb_archive, ndk, abi)
    output_dir.mkdir(parents=True, exist_ok=True)
    if output_dir.is_symlink() or not output_dir.is_dir():
        fail("output directory must be a real directory")
    archive_path = output_dir / f"asicen-userland-{VERSION}-android-{abi}.tar.gz"
    if archive_path.exists() or archive_path.is_symlink():
        fail(f"output archive already exists: {archive_path}")

    with tempfile.TemporaryDirectory(prefix=".asicen-android-stage-",
                                     dir=output_dir) as temporary:
        stage = Path(temporary) / "candidate"
        (stage / "bin").mkdir(parents=True)
        (stage / "packaging/termux").mkdir(parents=True)
        (stage / "licenses").mkdir(parents=True)
        for name in COMMANDS:
            shutil.copyfile(commands / f"{name}-{abi}", stage / "bin" / name)
            (stage / "bin" / name).chmod(0o755)
        launcher = stage / "packaging/termux/asicen-termux"
        shutil.copyfile(ROOT / "packaging/termux/asicen-termux", launcher)
        launcher.chmod(0o755)
        shutil.copyfile(ROOT / "COPYING.gpl2", stage / "LICENSE")
        shutil.copyfile(ROOT / "NOTICES.md", stage / "NOTICES.md")
        shutil.copyfile(ROOT / "README.md", stage / "README.md")
        (stage / "licenses/libusb-COPYING").write_bytes(libusb_copying(libusb_archive))
        shutil.copyfile(ndk / "NOTICE", stage / "licenses/ndk-NOTICE")
        shutil.copyfile(ndk / "source.properties", stage / "licenses/ndk-source.properties")
        (stage / "REBUILD.md").write_text(rebuild_instructions(abi), encoding="utf-8")
        (stage / "BUILD-METADATA.txt").write_text(
            f"Archive: asicen-userland {VERSION} Android {abi}; private candidate, not a release.\n"
            f"Android API: 24\n"
            f"Android ABI: {abi}\n"
            f"Interpreter: {ABI_INFO[abi]}\n"
            f"libusb: 1.0.30 static\n"
            "Firmware: excluded\n",
            encoding="utf-8")
        for path in stage.rglob("*"):
            if path.is_file():
                path.chmod(0o755 if path.name in COMMANDS or path.name == "asicen-termux"
                           else 0o644)
        write_reproducible_archive(stage, archive_path)

    try:
        verify_archive(archive_path, abi)
    except (ValueError, OSError, tarfile.TarError) as exc:
        archive_path.unlink(missing_ok=True)
        raise
    return archive_path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--abi", required=True, choices=sorted(ABI_INFO))
    parser.add_argument("--commands", required=True, type=Path)
    parser.add_argument("--libusb-archive", required=True, type=Path)
    parser.add_argument("--ndk", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        archive = assemble(args.commands.resolve(strict=True),
                           args.libusb_archive.resolve(strict=True),
                           args.ndk.resolve(strict=True), args.output, args.abi)
    except (ValueError, OSError, subprocess.CalledProcessError,
            tarfile.TarError) as exc:
        print(f"package failed: {exc}", file=sys.stderr)
        return 1
    print(f"archive={archive}")
    print(f"sha256={digest(archive)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
