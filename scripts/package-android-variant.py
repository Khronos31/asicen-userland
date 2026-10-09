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
FIRMWARE_SHA256 = "b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8"
FIRMWARE_NOTICE = (
    "Vendor component: PLEX loader firmware, distributed separately from this project's license.\n"
    "Source artifact SHA-256: 11a84eaef0157ac08c0b4128aa622a625e8914a28c59ef06e76378ee6094c5de\n"
    "Original loader object: loader.ko, SHA-256 10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0\n"
    f"Extracted component: FirmBin, 16384 bytes, SHA-256 {FIRMWARE_SHA256}\n"
    "Redistribution rights: unresolved. Inclusion reflects the user's explicit candidate-distribution choice and does not assert that rights are cleared.\n"
    "This file is not relicensed under GPL, MIT, public domain, or this project's terms.\n"
)


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


LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"


def rebuild_instructions(abi: str, source_sha: str, ndk_revision: str) -> str:
    return (
        "# Rebuild this Android command archive\n\n"
        f"These commands were built for Android API 24 with static libusb 1.0.30\n"
        f"and the NDK's static libc++, targeting ABI {abi}.\n"
        f"NDK Pkg.Revision recorded with this archive: {ndk_revision}\n"
        f"Project source snapshot SHA-256: {source_sha}\n"
        f"libusb archive SHA-256: {LIBUSB_SHA256}\n"
        "The snapshot is the corresponding source for this checkout. It omits\n"
        "firmware/. firmware/asicen-loader.bin in this binary archive is a separate\n"
        "vendor component. Redistribution rights for that file are unresolved.\n\n"
        "Release build, using the pinned libusb archive in source/:\n\n"
        "    export ANDROID_NDK_HOME=/path/to/android-ndk-r27\n"
        "    tar -xzf source/asicen-userland-source.tar.gz -C project-src\n"
        "    sh project-src/scripts/build-android.sh --abi " + abi + " \\\n"
        "        --output ../android-" + abi + " \\\n"
        "        --libusb-archive source/libusb-1.0.30.tar.bz2\n\n"
        "build-android.sh rejects any libusb archive whose SHA-256 is not the pin\n"
        "above. A modified-libusb relink therefore does not go through that check.\n"
        "Unpack source/libusb-1.0.30.tar.bz2, change it, and build the static\n"
        "library with the same NDK clang, --host, --disable-shared and --enable-static\n"
        "flags that build-android.sh uses. Then configure the unpacked project with\n"
        "the NDK toolchain file, ANDROID_PLATFORM=android-24, c++_static, and:\n\n"
        "    -DASICEN_LIBUSB_INCLUDE_DIR=$prefix/include/libusb-1.0\n"
        "    -DASICEN_LIBUSB_LIBRARY=$prefix/lib/libusb-1.0.a\n\n"
        "Build the asicend, asicenctl and asicen-ts targets. scripts/test-android-libusb-relink.sh\n"
        "performs that path for one ABI: it changes LIBUSB_NANO from 12037 to 12038\n"
        "and requires 12038 in the unstripped asicend binary.\n"
        "Each command must pass scripts/verify-android-elf.sh.\n"
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
        firmware = unpacked / "firmware/asicen-loader.bin"
        notice = unpacked / "licenses/VENDOR-FIRMWARE-NOTICE.txt"
        if firmware.is_symlink() or not firmware.is_file():
            fail("archive does not contain firmware/asicen-loader.bin")
        if digest(firmware) != FIRMWARE_SHA256:
            fail("archive firmware does not match the pinned loader")
        if notice.is_symlink() or not notice.is_file() or FIRMWARE_SHA256 not in notice.read_text(encoding="utf-8"):
            fail("archive vendor firmware notice is missing")
        source = unpacked / "source/asicen-userland-source.tar.gz"
        libusb = unpacked / "source/libusb-1.0.30.tar.bz2"
        toolchain_notice = unpacked / "licenses/ndk-NOTICE.toolchain"
        metadata = (unpacked / "BUILD-METADATA.txt").read_text(encoding="utf-8")
        if source.is_symlink() or not source.is_file():
            fail("archive does not contain the project source snapshot")
        source_sha = digest(source)
        if source_sha not in metadata:
            fail("archive metadata does not name the source snapshot hash")
        with tarfile.open(source, "r:gz") as snapshot:
            leaked = [name for name in snapshot.getnames()
                      if name == "firmware" or name.startswith("firmware/")
                      or name.endswith("asicen-loader.bin")]
        if leaked:
            fail("project source snapshot contains firmware")
        if libusb.is_symlink() or not libusb.is_file() or digest(libusb) != LIBUSB_SHA256:
            fail("archive libusb source does not match the pinned archive")
        if toolchain_notice.is_symlink() or not toolchain_notice.is_file() or toolchain_notice.stat().st_size == 0:
            fail("archive does not contain the NDK toolchain notice")
        if "NOTICE.toolchain" not in metadata:
            fail("archive metadata does not name the NDK toolchain notice")


def checked_firmware(path: Path) -> Path:
    if path.is_symlink() or not path.is_file():
        fail("firmware must be a regular non-symlink file")
    resolved = path.resolve(strict=True)
    if resolved.stat().st_size != 16384 or digest(resolved) != FIRMWARE_SHA256:
        fail("firmware does not match the pinned Linux loader")
    return resolved


def ndk_revision(ndk: Path) -> str:
    props = ndk / "source.properties"
    for line in props.read_text(encoding="utf-8").splitlines():
        if line.startswith("Pkg.Revision = "):
            return line.split("=", 1)[1].strip()
    fail("NDK source.properties has no Pkg.Revision")


def checked_source_archive(path: Path) -> Path:
    if path.is_symlink() or not path.is_file():
        fail("source snapshot must be a regular non-symlink file")
    resolved = path.resolve(strict=True)
    with tarfile.open(resolved, "r:gz") as snapshot:
        leaked = [name for name in snapshot.getnames()
                  if name == "firmware" or name.startswith("firmware/")
                  or name.endswith("asicen-loader.bin")]
    if leaked:
        fail("source snapshot contains firmware")
    return resolved


def assemble(commands: Path, libusb_archive: Path, ndk: Path, output_dir: Path,
             abi: str, firmware: Path, source_archive: Path) -> Path:
    validate_inputs(commands, libusb_archive, ndk, abi)
    firmware = checked_firmware(firmware)
    source_archive = checked_source_archive(source_archive)
    if digest(libusb_archive) != LIBUSB_SHA256:
        fail("libusb archive does not match the pinned 1.0.30 archive")
    toolchain_notice = ndk / "NOTICE.toolchain"
    if toolchain_notice.is_symlink() or not toolchain_notice.is_file():
        fail("NDK NOTICE.toolchain is missing")
    revision = ndk_revision(ndk)
    source_sha = digest(source_archive)
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
        (stage / "source").mkdir(parents=True)
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
        shutil.copyfile(toolchain_notice, stage / "licenses/ndk-NOTICE.toolchain")
        shutil.copyfile(ndk / "source.properties", stage / "licenses/ndk-source.properties")
        shutil.copyfile(source_archive, stage / "source/asicen-userland-source.tar.gz")
        shutil.copyfile(libusb_archive, stage / "source/libusb-1.0.30.tar.bz2")
        (stage / "firmware").mkdir()
        shutil.copyfile(firmware, stage / "firmware/asicen-loader.bin")
        (stage / "licenses/VENDOR-FIRMWARE-NOTICE.txt").write_text(
            FIRMWARE_NOTICE, encoding="utf-8")
        (stage / "REBUILD.md").write_text(
            rebuild_instructions(abi, source_sha, revision), encoding="utf-8")
        (stage / "BUILD-METADATA.txt").write_text(
            f"Archive: asicen-userland {VERSION} Android {abi}; private candidate, not a release.\n"
            f"Android API: 24\n"
            f"Android ABI: {abi}\n"
            f"Interpreter: {ABI_INFO[abi]}\n"
            f"libusb: 1.0.30 static\n"
            f"libusb archive SHA-256: {LIBUSB_SHA256}\n"
            f"Source snapshot SHA-256: {source_sha}\n"
            f"NDK Pkg.Revision: {revision}\n"
            "NDK runtime notice: licenses/ndk-NOTICE.toolchain\n"
            f"Firmware SHA-256: {FIRMWARE_SHA256}\n"
            "Firmware redistribution rights: unresolved\n",
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
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--source-archive", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        archive = assemble(args.commands.resolve(strict=True),
                           args.libusb_archive.resolve(strict=True),
                           args.ndk.resolve(strict=True), args.output, args.abi,
                           args.firmware, args.source_archive)
    except (ValueError, OSError, subprocess.CalledProcessError,
            tarfile.TarError) as exc:
        print(f"package failed: {exc}", file=sys.stderr)
        return 1
    print(f"archive={archive}")
    print(f"sha256={digest(archive)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
