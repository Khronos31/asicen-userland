#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Assemble and audit one private Linux static-command/host-IFD variant."""
import argparse
import gzip
import hashlib
import importlib.util
from pathlib import Path, PurePosixPath
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AUDIT_PATH = ROOT / "scripts" / "audit-linux-candidate.py"
SPEC = importlib.util.spec_from_file_location("asicen_static_audit", AUDIT_PATH)
STATIC_AUDIT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(STATIC_AUDIT)

FIRMWARE_SHA256 = STATIC_AUDIT.FIRMWARE_SHA256
IFD_EXPORTS = (
    "IFDHCreateChannel", "IFDHCreateChannelByName", "IFDHCloseChannel",
    "IFDHGetCapabilities", "IFDHSetCapabilities", "IFDHSetProtocolParameters",
    "IFDHPowerICC", "IFDHTransmitToICC", "IFDHControl", "IFDHICCPresence",
)
READER_TEMPLATE = "packaging/pcsc/reader.conf.d/asicen-reader.conf.in"


def fail(message: str) -> None:
    raise ValueError(message)


def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def machine_of(path: Path) -> str:
    output = subprocess.run(["readelf", "-h", str(path)], check=True,
                            text=True, stdout=subprocess.PIPE).stdout
    match = re.search(r"^\s*Machine:\s*(.+)$", output, re.MULTILINE)
    if not match:
        fail(f"ELF machine is missing: {path}")
    return match.group(1).strip()


def allowed_ifd_needed(variant: str, machine: str) -> set[str]:
    if variant == "glibc":
        if machine == "AArch64":
            return {"libc.so.6", "libpthread.so.0"}
        if machine == "Advanced Micro Devices X86-64":
            return {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"}
        fail(f"unsupported glibc IFD machine: {machine}")
    if variant == "musl":
        if machine == "AArch64":
            return {"libc.musl-aarch64.so.1"}
        if machine == "Advanced Micro Devices X86-64":
            return {"libc.musl-x86_64.so.1"}
        fail(f"unsupported musl IFD machine: {machine}")
    fail(f"unsupported IFD libc variant: {variant}")


def validate_ifd_needed(needed: set[str], variant: str, machine: str) -> None:
    allowed = allowed_ifd_needed(variant, machine)
    if needed != allowed:
        fail(f"IFD dynamic dependency allowlist mismatch: {sorted(needed)}")


def validate_ifd(directory: Path, variant: str, source_sha: str,
                 expected_machine: str) -> Path:
    if directory.is_symlink() or not directory.is_dir():
        fail("IFD candidate must be a real directory")
    plugin = directory / "libifd-asicen.so"
    metadata_path = directory / "BUILD-METADATA.txt"
    sums_path = directory / "SHA256SUMS"
    for path in (plugin, metadata_path, sums_path):
        if path.is_symlink() or not path.is_file():
            fail(f"missing or unsafe IFD candidate item: {path.name}")
    metadata = metadata_path.read_text(encoding="utf-8")
    expected_variant = "glibc-2.31" if variant == "glibc" else "musl"
    if f"Libc variant: {expected_variant}" not in metadata:
        fail(f"IFD candidate does not match requested {variant} variant")
    if f"Source archive SHA-256: {source_sha}" not in metadata:
        fail("IFD was not built from the same immutable source snapshot as the commands")
    if machine_of(plugin) != expected_machine:
        fail("IFD architecture does not match static command architecture")
    dynamic = subprocess.run(["readelf", "-d", str(plugin)], check=True,
                             text=True, stdout=subprocess.PIPE).stdout
    if "NEEDED" not in dynamic:
        fail("IFD plugin has no dynamic host dependency; expected host PC/SC/libc linkage")
    needed = set(re.findall(r"Shared library: \[([^]]+)\]", dynamic))
    validate_ifd_needed(needed, variant, expected_machine)
    versions = subprocess.run(["readelf", "-V", str(plugin)], check=True,
                              text=True, stdout=subprocess.PIPE).stdout
    glibc_versions = sorted(set(re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", versions)))
    if variant == "musl" and glibc_versions:
        fail("musl IFD has glibc version requirements")
    if variant == "glibc":
        for value in glibc_versions:
            major, minor = (int(part) for part in value.split(".")[:2])
            if major > 2 or (major == 2 and minor > 31):
                fail(f"glibc IFD requires GLIBC_{value}, above the 2.31 baseline")
    symbols = subprocess.run(["nm", "-D", "--defined-only", str(plugin)], check=True,
                             text=True, stdout=subprocess.PIPE).stdout
    for symbol in IFD_EXPORTS:
        if not re.search(rf"\b_?{re.escape(symbol)}$", symbols, re.MULTILINE):
            fail(f"IFD export missing: {symbol}")
    sums = STATIC_AUDIT.parse_sums(sums_path)
    actual_sums = {item.relative_to(directory).as_posix(): digest(item)
                   for item in directory.rglob("*")
                   if item.is_file() and item.name != "SHA256SUMS"}
    if sums != actual_sums:
        fail("IFD artifact SHA256SUMS does not match its contents")
    return plugin


def source_member(source_archive: Path, member_path: str) -> bytes:
    with tarfile.open(source_archive, "r:gz") as archive:
        member = archive.getmember(member_path)
        stream = archive.extractfile(member)
        if stream is None:
            fail(f"source archive member is not a regular file: {member_path}")
        return stream.read()


def make_firmware_notice(firmware_hash: str) -> str:
    return (
        "Vendor component: PLEX loader firmware, distributed separately from this project's license.\n"
        "Source artifact SHA-256: 11a84eaef0157ac08c0b4128aa622a625e8914a28c59ef06e76378ee6094c5de\n"
        "Original loader object: loader.ko, SHA-256 10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0\n"
        f"Extracted component: FirmBin, 16384 bytes, SHA-256 {firmware_hash}\n"
        "Redistribution rights: unresolved. Inclusion reflects the user's explicit candidate-distribution choice and does not assert that rights are cleared.\n"
        "This file is not relicensed under GPL, MIT, public domain, or this project's terms.\n"
    )


def validate_firmware_path(path: Path, repository_root: Path = ROOT) -> Path:
    if path.is_symlink() or not path.is_file():
        fail("firmware must be a regular non-symlink external file")
    resolved = path.resolve(strict=True)
    if resolved.is_relative_to(repository_root.resolve()):
        fail("vendor firmware input must remain outside the source checkout")
    if resolved.stat().st_size != 16384 or digest(resolved) != FIRMWARE_SHA256:
        fail("firmware input does not match the recorded vendor component")
    return resolved


def validate_firmware_notice(path: Path) -> None:
    if path.is_symlink() or not path.is_file():
        fail("final candidate lacks the vendor firmware notice")
    notice = path.read_text(encoding="utf-8")
    if FIRMWARE_SHA256 not in notice or "Redistribution rights: unresolved" not in notice:
        fail("vendor firmware notice does not identify the recorded component and rights status")


def validate_output_path(output: Path, repository_root: Path = ROOT) -> Path:
    if output.exists() or output.is_symlink() or Path(str(output) + ".tar.gz").exists() or \
            Path(str(output) + ".tar.gz").is_symlink() or \
            Path(str(output) + ".tar.gz.sha256").exists():
        fail("final output path already exists")
    parent = output.parent.resolve(strict=True)
    normalized = parent / output.name
    if normalized.is_relative_to(repository_root.resolve()):
        fail("final package must be written outside the source checkout")
    return normalized


def require_final_files(root: Path) -> None:
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
        path = root / relative
        if path.is_symlink() or not path.is_file():
            fail(f"final candidate lacks required file: {relative}")


def require_source_metadata(path: Path, source_sha: str) -> None:
    if path.is_symlink() or not path.is_file():
        fail(f"retained build metadata is missing: {path.name}")
    if f"Source archive SHA-256: {source_sha}" not in path.read_text(encoding="utf-8"):
        fail("retained command/IFD build metadata does not match the packaged source archive")


def expected_final_inventory(variant: str) -> set[str]:
    expected = {f"bin/{name}" for name in STATIC_AUDIT.COMMANDS}
    expected.update({"source/asicen-userland-source.tar.gz",
                     "source/libusb-1.0.30.tar.bz2",
                     "source/asicen-linux-corresponding-source.tar.gz",
                     "SOURCE-MANIFEST.txt", "REBUILD.md", "BUILD-METADATA.txt",
                     "SHA256SUMS", "firmware/asicen-loader.bin",
                     "licenses/VENDOR-FIRMWARE-NOTICE.txt",
                     "licenses/IFD-BUILD-METADATA.txt",
                     "licenses/COMMAND-BUILD-METADATA.txt",
                     "licenses/IFD-GCC-COPYING3.txt",
                     "licenses/IFD-GCC-COPYING.RUNTIME.txt",
                     "lib/pcsc/ifd-asicen.so",
                     "pcsc/reader.conf.d/asicen-reader.conf.in"})
    expected.update(STATIC_AUDIT.LICENSES)
    if variant == "musl":
        expected.add("licenses/IFD-musl-COPYRIGHT.txt")
    return expected


def write_reproducible_archive(root: Path, archive_path: Path) -> None:
    raw_tar = Path(str(archive_path) + ".rawtar")
    with tarfile.open(raw_tar, "w") as tar:
        for item in [root, *sorted(root.rglob("*"))]:
            info = tar.gettarinfo(str(item), arcname=item.relative_to(root.parent).as_posix())
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


def candidate_files(root: Path) -> list[Path]:
    paths = []
    for path in root.rglob("*"):
        if path.is_symlink():
            fail(f"candidate staging contains a symlink: {path.relative_to(root)}")
        if path.is_file() and path.name != "SHA256SUMS":
            paths.append(path)
    return sorted(paths, key=lambda item: item.relative_to(root).as_posix())


def audit_final(root: Path, variant: str) -> dict[str, str]:
    if root.is_symlink() or not root.is_dir():
        fail("final candidate must be a real directory")
    metadata = (root / "BUILD-METADATA.txt").read_text(encoding="utf-8")
    if f"Candidate class: final Linux {variant} IFD variant" not in metadata:
        fail("metadata does not identify the requested final IFD variant")
    arch = re.search(r"^Architecture: (x86_64|aarch64)$", metadata, re.MULTILINE)
    if not arch:
        fail("final candidate architecture is missing or unsupported")
    expected_machine = {"x86_64": "Advanced Micro Devices X86-64",
                        "aarch64": "AArch64"}[arch.group(1)]
    sums = STATIC_AUDIT.parse_sums(root / "SHA256SUMS")
    actual = {path.relative_to(root).as_posix() for path in candidate_files(root)}
    expected_inventory = expected_final_inventory(variant) - {"SHA256SUMS"}
    if actual != expected_inventory or set(sums) != expected_inventory:
        fail("final candidate inventory mismatch: "
             f"missing={sorted(expected_inventory-actual)} "
             f"extra={sorted(actual-expected_inventory)} "
             f"checksum-extra={sorted(set(sums)-expected_inventory)}")
    STATIC_AUDIT.audit_candidate_modes(root, include_ifd=True)
    for rel, expected in sums.items():
        path = root / rel
        if digest(path) != expected:
            fail(f"final candidate checksum mismatch: {rel}")
    for command in ("asicend", "asicen-ts", "asicenctl"):
        path = root / "bin" / command
        STATIC_AUDIT.verify_static_elf(path, expected_machine)
        subprocess.run([str(path), "--help"], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    plugin = root / "lib/pcsc/ifd-asicen.so"
    if machine_of(plugin) != expected_machine:
        fail("final IFD architecture mismatch")
    dynamic = subprocess.run(["readelf", "-d", str(plugin)], check=True,
                             text=True, stdout=subprocess.PIPE).stdout
    if "NEEDED" not in dynamic:
        fail("final IFD is not a dynamically loaded host plugin")
    needed = set(re.findall(r"Shared library: \[([^]]+)\]", dynamic))
    validate_ifd_needed(needed, variant, expected_machine)
    versions = subprocess.run(["readelf", "-V", str(plugin)], check=True,
                              text=True, stdout=subprocess.PIPE).stdout
    glibc_versions = re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", versions)
    if variant == "musl" and glibc_versions:
        fail("musl IFD contains glibc symbol requirements")
    if variant == "glibc":
        for value in glibc_versions:
            major, minor = (int(part) for part in value.split(".")[:2])
            if major > 2 or (major == 2 and minor > 31):
                fail(f"final IFD exceeds glibc 2.31: GLIBC_{value}")
    symbols = subprocess.run(["nm", "-D", "--defined-only", str(plugin)], check=True,
                             text=True, stdout=subprocess.PIPE).stdout
    for symbol in IFD_EXPORTS:
        if not re.search(rf"\b_?{re.escape(symbol)}$", symbols, re.MULTILINE):
            fail(f"final IFD export missing: {symbol}")
    source = root / "source/asicen-userland-source.tar.gz"
    STATIC_AUDIT.audit_source_archive(source)
    require_final_files(root)
    firmware = root / "firmware/asicen-loader.bin"
    if digest(firmware) != FIRMWARE_SHA256:
        fail("final candidate firmware is missing or differs from the vendor notice")
    validate_firmware_notice(root / "licenses/VENDOR-FIRMWARE-NOTICE.txt")
    if not (root / "licenses/IFD-GCC-COPYING3.txt").is_file() or \
            not (root / "licenses/IFD-GCC-COPYING.RUNTIME.txt").is_file():
        fail("final candidate lacks IFD toolchain runtime licenses")
    if variant == "musl" and not (root / "licenses/IFD-musl-COPYRIGHT.txt").is_file():
        fail("final musl IFD candidate lacks the musl license text")
    for path in (root / "licenses/IFD-BUILD-METADATA.txt",
                 root / "licenses/COMMAND-BUILD-METADATA.txt"):
        if not path.is_file() or digest(path) not in metadata:
            fail("final candidate does not retain command and IFD build metadata")
        require_source_metadata(path, digest(source))
    if f"Command source archive SHA-256: {digest(source)}" not in metadata or \
            f"IFD build metadata SHA-256: {digest(root / 'licenses/IFD-BUILD-METADATA.txt')}" not in metadata or \
            f"Static command candidate metadata SHA-256: {digest(root / 'licenses/COMMAND-BUILD-METADATA.txt')}" not in metadata:
        fail("final package metadata hashes do not match retained build metadata and source")
    STATIC_AUDIT.audit_corresponding_source_bundle(
        root / "source/asicen-linux-corresponding-source.tar.gz", root)
    return {"variant": variant, "architecture": arch.group(1), "status": "passed"}


def assemble(commands: Path, ifd_dir: Path, variant: str, firmware: Path,
             output: Path) -> Path:
    STATIC_AUDIT.audit_candidate(commands)
    commands = commands.resolve(strict=True)
    ifd_dir = ifd_dir.resolve(strict=True)
    firmware = validate_firmware_path(firmware)
    source_archive = commands / "source/asicen-userland-source.tar.gz"
    source_sha = digest(source_archive)
    meta = (commands / "BUILD-METADATA.txt").read_text(encoding="utf-8")
    arch_match = re.search(r"^Architecture: (x86_64|aarch64)$", meta, re.MULTILINE)
    if not arch_match:
        fail("static command architecture is missing")
    expected_machine = {"x86_64": "Advanced Micro Devices X86-64",
                        "aarch64": "AArch64"}[arch_match.group(1)]
    plugin = validate_ifd(ifd_dir, variant, source_sha, expected_machine)
    if firmware == commands / "firmware/asicen-loader.bin":
        fail("firmware input aliases the intermediate candidate")
    output.parent.mkdir(parents=True, exist_ok=True)
    output = validate_output_path(output)
    output_parent = output.parent
    with tempfile.TemporaryDirectory(prefix=".asicen-final-", dir=output_parent) as temporary:
        stage = Path(temporary) / "candidate"
        shutil.copytree(commands, stage, symlinks=False)
        shutil.rmtree(stage / "bin" / "__pycache__", ignore_errors=True)
        (stage / "firmware").mkdir()
        shutil.copyfile(firmware, stage / "firmware/asicen-loader.bin")
        (stage / "licenses/VENDOR-FIRMWARE-NOTICE.txt").write_text(
            make_firmware_notice(digest(firmware)), encoding="utf-8")
        (stage / "lib/pcsc").mkdir(parents=True)
        shutil.copyfile(plugin, stage / "lib/pcsc/ifd-asicen.so")
        shutil.copyfile(ifd_dir / "BUILD-METADATA.txt",
                        stage / "licenses/IFD-BUILD-METADATA.txt")
        shutil.copyfile(commands / "BUILD-METADATA.txt",
                        stage / "licenses/COMMAND-BUILD-METADATA.txt")
        for license_path in (ifd_dir / "licenses").glob("*"):
            if license_path.is_file() and not license_path.is_symlink():
                shutil.copyfile(license_path,
                                stage / "licenses" / f"IFD-{license_path.name}")
        (stage / "REBUILD.md").write_text(
            "# Rebuild this private Linux candidate\n\n"
            "These files are already at the corresponding-source bundle root after "
            "extracting it. Requirements: musl GCC/G++, a separate glibc 2.31/GCC 10.2.1 "
            "or musl/GCC 14.2.0 IFD toolchain, CMake, Ninja, GNU make/tar/coreutils, gzip, "
            "pkgconf, Linux headers, and matching PC/SC development files.\n\n"
            "    mkdir project-src\n"
            "    tar -xzf source/asicen-userland-source.tar.gz -C project-src\n"
            "    python3 project-src/scripts/audit-linux-candidate.py --source-archive source/asicen-userland-source.tar.gz\n"
            "\nBuild the pinned static commands in a musl toolchain and the matching IFD "
            "in its named host-libc root:\n\n"
            "    sh project-src/scripts/build-linux-static.sh --output ../commands "
            "--source-snapshot source/asicen-userland-source.tar.gz "
            "--libusb-archive source/libusb-1.0.30.tar.bz2 "
            "--musl-license licenses/musl-COPYRIGHT.txt "
            "--gcc-copying3 licenses/GCC-COPYING3.txt "
            "--gcc-runtime-exception licenses/GCC-COPYING.RUNTIME.txt\n"
            f"    sh project-src/scripts/build-linux-ifd.sh --libc {variant} "
            "--source-snapshot source/asicen-userland-source.tar.gz "
            "--gcc-copying3 licenses/GCC-COPYING3.txt "
            "--gcc-runtime-exception licenses/GCC-COPYING.RUNTIME.txt "
            "--musl-license licenses/musl-COPYRIGHT.txt --output ../ifd\n"
            "    python3 project-src/scripts/package-linux-variant.py --commands ../commands "
            f"--ifd ../ifd --libc {variant} --firmware /path/to/supplied-loader.bin "
            "--output ../candidate\n"
            "\nFor an LGPL relink with modified libusb, unpack and modify the included "
            "libusb source, then run these commands from the bundle root. The release "
            "script intentionally pins the pristine archive; this manual path uses the "
            "rebuilt library and does not need firmware:\n\n"
            "    mkdir modified-libusb modified-prefix\n"
            "    tar -xjf source/libusb-1.0.30.tar.bz2 -C modified-libusb --strip-components=1\n"
            "    cd modified-libusb\n"
            "    ./configure --prefix=\"$PWD/../modified-prefix\" --disable-shared --enable-static --with-pic --disable-udev --disable-examples-build --disable-tests-build --disable-dependency-tracking\n"
            "    make && make install\n"
            "    cd ..\n"
            "    cmake -S project-src -B modified-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DASICEN_ENABLE_LIBUSB=ON -DASICEN_ENABLE_IFD=OFF -DASICEN_BUILD_TESTS=OFF -DASICEN_LIBUSB_INCLUDE_DIR=\"$PWD/modified-prefix/include/libusb-1.0\" -DASICEN_LIBUSB_LIBRARY=\"$PWD/modified-prefix/lib/libusb-1.0.a\" -DASICEN_LIBUSB_EXTRA_LIBRARIES=pthread -DCMAKE_CXX_FLAGS='-static -static-libstdc++ -static-libgcc' -DCMAKE_EXE_LINKER_FLAGS='-static -static-libstdc++ -static-libgcc'\n"
            "    cmake --build modified-build --target asicend asicen-ts asicenctl\n\n"
            "The three rebuilt executables can be redistributed with modified libusb "
            "under the applicable LGPL terms. Recompute candidate checksums and metadata "
            "for any modified derivative; the pinned candidate audit will reject it by design.\n\n"
            "Firmware is a separate vendor input and is not in this source bundle. The "
            "final assembler requires a separately supplied file matching the notice hash. "
            "This is a private candidate, not a release.\n",
            encoding="utf-8")
        (stage / "pcsc/reader.conf.d").mkdir(parents=True)
        template = source_member(source_archive, READER_TEMPLATE)
        (stage / "pcsc/reader.conf.d/asicen-reader.conf.in").write_bytes(template)
        (stage / "BUILD-METADATA.txt").write_text(
            "Candidate class: final Linux " + variant + " IFD variant; private candidate, not a release.\n" +
            f"Libc variant: {'glibc-2.31' if variant == 'glibc' else 'musl'}\n" +
            f"Architecture: {arch_match.group(1)}\n" +
            f"Command source archive SHA-256: {source_sha}\n" +
            f"IFD build metadata SHA-256: {digest(ifd_dir / 'BUILD-METADATA.txt')}\n" +
            f"Static command candidate metadata SHA-256: {digest(commands / 'BUILD-METADATA.txt')}\n" +
            f"Firmware size: 16384\nFirmware SHA-256: {digest(firmware)}\n" +
            "The commands are musl-static; the host-loaded IFD uses the separately named host libc variant.\n",
            encoding="utf-8")
        STATIC_AUDIT.normalize_candidate_modes(stage)
        bundle_path = stage / "source/asicen-linux-corresponding-source.tar.gz"
        with tempfile.TemporaryDirectory(prefix="asicen-bundle-") as bundle_temp:
            raw_tar = Path(bundle_temp) / "source.tar"
            bundle_items = [stage / "source/asicen-userland-source.tar.gz",
                            stage / "source/libusb-1.0.30.tar.bz2",
                            stage / "SOURCE-MANIFEST.txt", stage / "REBUILD.md",
                            *sorted((stage / "licenses").glob("*"))]
            with tarfile.open(raw_tar, "w") as bundle:
                for item in bundle_items:
                    info = bundle.gettarinfo(str(item), arcname=item.relative_to(stage).as_posix())
                    info.mtime = 0
                    info.uid = info.gid = 0
                    info.uname = info.gname = ""
                    with item.open("rb") as content:
                        bundle.addfile(info, content)
            with raw_tar.open("rb") as source_stream, bundle_path.open("wb") as target:
                subprocess.run(["gzip", "-n", "-9"], stdin=source_stream,
                               stdout=target, check=True)
        STATIC_AUDIT.normalize_candidate_modes(stage)
        lines = []
        for item in candidate_files(stage):
            lines.append(f"{digest(item)}  {item.relative_to(stage).as_posix()}")
        (stage / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")
        STATIC_AUDIT.normalize_candidate_modes(stage)
        audit_final(stage, variant)
        archive = Path(temporary) / "candidate.tar.gz"
        write_reproducible_archive(stage, archive)
        staged_archive = Path(str(output) + ".tar.gz")
        shutil.copytree(stage, output)
        shutil.copyfile(archive, staged_archive)
        staged_archive.chmod(0o644)
        staged_archive.with_suffix(staged_archive.suffix + ".sha256").write_text(
            f"{digest(staged_archive)}  {staged_archive.name}\n", encoding="ascii")
        staged_archive.with_suffix(staged_archive.suffix + ".sha256").chmod(0o644)
    print(f"private final Linux {variant} candidate: {output}")
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--commands", required=True, type=Path)
    parser.add_argument("--ifd", required=True, type=Path)
    parser.add_argument("--libc", required=True, choices=("glibc", "musl"))
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        candidate = assemble(args.commands, args.ifd, args.libc,
                             args.firmware, args.output)
        result = audit_final(candidate, args.libc)
    except (ValueError, OSError, subprocess.CalledProcessError,
            tarfile.TarError) as exc:
        print(f"package failed: {exc}", file=sys.stderr)
        return 1
    for key, value in result.items():
        print(f"{key}={value}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
