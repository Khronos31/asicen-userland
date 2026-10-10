#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# Adapted from px4-userland 1a1485d0c3e972e0a47be907edb67949564aa9a7.
# Local changes: ASICEN names, firmware policy, and retained runtime licenses.
"""Audit asicen-userland binaries and deterministic release archives."""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import plistlib
import re
import select
import signal
import socket
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import zipfile


PLATFORMS = {
    "linux-glibc-x86_64": "linux-glibc",
    "linux-glibc-aarch64": "linux-glibc",
    "linux-musl-x86_64": "linux-musl",
    "linux-musl-aarch64": "linux-musl",
    "darwin-arm64": "darwin",
    "android-aarch64": "android",
    "android-armv7a": "android",
    "android-x86_64": "android",
    "windows-x86_64": "windows",
}
LINUX_TARGETS = {
    "linux-glibc-x86_64": {
        "machine": "Advanced Micro Devices X86-64",
        "libc": "libc.so.6", "floor": "2.31",
        "needed": {"libc.so.6", "libpthread.so.0", "ld-linux-x86-64.so.2"},
    },
    "linux-glibc-aarch64": {
        "machine": "AArch64",
        "libc": "libc.so.6", "floor": "2.31",
        "needed": {"libc.so.6", "libpthread.so.0", "ld-linux-aarch64.so.1"},
    },
    "linux-musl-x86_64": {
        "machine": "Advanced Micro Devices X86-64",
        "libc": "libc.musl-x86_64.so.1", "floor": None, "needed": None,
    },
    "linux-musl-aarch64": {
        "machine": "AArch64",
        "libc": "libc.musl-aarch64.so.1", "floor": None, "needed": None,
    },
}
VERSION_RE = re.compile(r"^[0-9]+\.[0-9]+\.[0-9]+$")
LIBUSB_SHA256 = "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf"
LIBUSB_COPYING_SHA256 = "5df07007198989c622f5d41de8d703e7bef3d0e79d62e24332ee739a452af62a"
FIRMWARE_SHA256 = "b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8"
FIRMWARE_SIZE = 16384
FIRMWARE_MEMBER = "firmware/asicen-loader.bin"
FIRMWARE_NOTICE_MEMBER = "licenses/VENDOR-FIRMWARE-NOTICE.txt"
FIRMWARE_NOTICE = (
    "Vendor component: PLEX loader firmware, distributed separately from this project's license.\n"
    "Source artifact SHA-256: 11a84eaef0157ac08c0b4128aa622a625e8914a28c59ef06e76378ee6094c5de\n"
    "Original loader object: loader.ko, SHA-256 10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0\n"
    f"Extracted component: FirmBin, {FIRMWARE_SIZE} bytes, SHA-256 {FIRMWARE_SHA256}\n"
    "Redistribution rights: unresolved. Inclusion reflects the user's explicit candidate-distribution choice and does not assert that rights are cleared.\n"
    "This file is not relicensed under GPL, MIT, public domain, or this project's terms.\n"
)
LINUX_RUNTIME_LICENSE_SHA256 = {
    "toolchain/gcc-COPYING3": "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903",
    "toolchain/gcc-COPYING.RUNTIME": "9d6b43ce4d8de0c878bf16b54d8e7a10d9bd42b75178153e3af6a815bdc90f74",
    "toolchain/musl-1.2.5-COPYRIGHT": "f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af",
}
LINUX_BUILD_METADATA_MEMBER = "evidence/linux-static-build.json"
LINUX_IFD_METADATA_MEMBER = "evidence/linux-ifd-build.json"
COMMON = {
    "LICENSE",
    "README.md",
    "THIRD_PARTY_NOTICES.md",
    "DEPENDENCY-NOTICE.txt",
    "libusb/COPYING",
    "manifest.json",
    "SHA256SUMS",
    "evidence/binary-audit.json",
    FIRMWARE_MEMBER,
    FIRMWARE_NOTICE_MEMBER,
}
LINUX_MDEV = {
    "mdev/asicen-userland-mdev.conf", "mdev/asicen-userland-mdev.sh", "mdev/asicen-userland-mdev.start",
}
PROGRAMS = ("asicend", "asicen-ts", "asicenctl")
TERMUX_LAUNCHER = "asicen-termux"

# Windows Phase 1 PE contract: ABI, static libusb marker, permitted import
# closure, and the exact zip member allowlist. libusb-1.0.dll is not shipped.
WINDOWS_PLATFORM = "windows-x86_64"
WINDOWS_LIBUSB_DLL = "libusb-1.0.dll"
WINDOWS_LIBUSB_MARKER = b"https://libusb.info"
WINDOWS_PROGRAM_ARTIFACTS = tuple(f"{program}.exe" for program in PROGRAMS)
IMAGE_FILE_MACHINE_AMD64 = 0x8664
IMAGE_FILE_DLL = 0x2000
PE32_PLUS_MAGIC = 0x20B
IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE = 0x0040
IMAGE_DLLCHARACTERISTICS_NX_COMPAT = 0x0100
# Only System32 components are allowed. libusb is inside asicend.exe. libc++,
# winpthread, and the MinGW runtime are statically linked, so their DLLs are
# rejected. UCRT and other api-ms-win-crt imports stay dynamic.
WINDOWS_SYSTEM_IMPORTS = {
    "advapi32.dll", "bcrypt.dll", "kernel32.dll", "ole32.dll",
    "shell32.dll", "user32.dll", "ws2_32.dll",
}
WINDOWS_UCRT_IMPORT_PREFIX = "api-ms-win-crt-"
# Exact narrow license texts for the statically linked llvm-mingw runtimes
# (LLVM/libc++/libunwind, MinGW-w64, winpthreads, winstorecompat). They are
# sourced from the pinned, checksum-verified llvm-mingw toolchain archive and
# are the only toolchain material shipped; no compiler binary or source bulk.
WINDOWS_TOOLCHAIN_LICENSE_SHA256 = {
    "toolchain/LICENSE.TXT": "8d85c1057d742e597985c7d4e6320b015a9139385cff4cbae06ffc0ebe89afee",
    "toolchain/mingw32/COPYING": "99a69660981156c21336fdb5661f89341b013c94e4bf9e1c7467b4745718397f",
    "toolchain/mingw32/COPYING.MinGW-w64-runtime.txt": "e9b2dc02451ea29092a1f25fa0f3c07207ed421f1807dffb0c4e6dce69dee7bd",
    "toolchain/mingw32/COPYING.MinGW-w64.txt": "f38e6194bd3bfa1b654f118e5acefe0aead437bbe669eee43957ccc65a7127f1",
    "toolchain/mingw32/COPYING.winpthreads.txt": "63263614cdd29f2f93cba85e992f041b31f9fc7b4033692f31269489a8a1b177",
    "toolchain/mingw32/COPYING.winstorecompat.txt": "fa7368d1f93d890c173177475d8ec7aff265a7d08f1f364d66f74d138978ce36",
}
WINDOWS_TOOLCHAIN_LICENSE_MEMBERS = frozenset(WINDOWS_TOOLCHAIN_LICENSE_SHA256)
WINDOWS_TOOLCHAIN_LICENSE_NOTICE = ",".join(sorted(WINDOWS_TOOLCHAIN_LICENSE_MEMBERS))
WINDOWS_ARCHIVE_MEMBERS = (set(COMMON) | set(WINDOWS_PROGRAM_ARTIFACTS)
                           | set(WINDOWS_TOOLCHAIN_LICENSE_MEMBERS))
DARWIN_IFD_ARTIFACT = "ifd/asicen-userland-ifd.bundle/Contents/MacOS/libasicen-userland-ifd.dylib"
BINARY_EXECUTABLE_MEMBERS = set(PROGRAMS) | set(WINDOWS_PROGRAM_ARTIFACTS) | {
    TERMUX_LAUNCHER, "ifd/asicen-userland-ifd.so", DARWIN_IFD_ARTIFACT,
    "mdev/asicen-userland-mdev.sh", "mdev/asicen-userland-mdev.start",
}
IFD_EXPORTS = (
    "IFDHCreateChannel", "IFDHCreateChannelByName", "IFDHCloseChannel",
    "IFDHGetCapabilities", "IFDHSetCapabilities", "IFDHSetProtocolParameters",
    "IFDHPowerICC", "IFDHTransmitToICC", "IFDHControl", "IFDHICCPresence",
)
READER_PLACEHOLDERS = {
    "@ASICEN_RUNTIME_DIR@",
    "@ASICEN_INSTANCE@",
    "@ASICEN_ACCESS@",
    "@ASICEN_IFD_LIBRARY@",
}
ANDROID_INVENTORY_MEMBERS = tuple(
    f"evidence/inventory/{program}-static-archives.tsv" for program in PROGRAMS
)
FORBIDDEN = re.compile(
    r"(^|/)(?:firmware|windows|win32|vendor|drivers?|dkms|kernel|apk|addon|add-on)(?:/|$)"
    r"|(?:\.apk$|\.ko$|\.sys$|\.inf$|\.dll$|\.exe$|\.bin$)"
    r"|(?:asicen-ts-probe|asicen-.*-probe)",
    re.IGNORECASE,
)
SOURCE_FORBIDDEN = re.compile(
    r"(^|/)(?:\.git|__pycache__|build(?:-[^/.]+)?|out|dist|firmware|vendor|drivers?|dkms|kernel|apk|addon|add-on)(?:/|$)"
    r"|(?:\.o$|\.a$|\.so(?:\.|$)|\.dylib$|\.apk$|\.ko$|\.sys$|\.inf$|\.dll$|\.exe$|\.bin$|\.py[co]$|\.pyd$)"
    r"|(?:asicen-ts-probe|asicen-.*-probe)", re.IGNORECASE
)
# Windows Phase 1 source lives only in the narrow adapter/test/build/doc paths.
# Any other windows/win32-segment member is legacy or vendor material and is
# rejected, so this is not a blanket allow for old Windows trees.
SOURCE_WINDOWS_TOKEN = re.compile(r"(^|/)(?:windows|win32)(?:/|$)", re.IGNORECASE)
WINDOWS_SOURCE_ALLOWED = re.compile(
    r"^repository/(?:"
    r"third_party/px4-userland/userland/src/windows/[\w./+-]+"
    r"|userland/src/windows/libusb_stream_policy\.(?:h|cpp)"
    r"|userland/tools/[\w.+-]*windows[\w.+-]*"
    r"|userland/tests/windows_[\w./+-]+"
    r"|third_party/px4-userland/userland/include/px4/[\w.+-]*windows[\w.+-]*"
    r"|scripts/build-windows\.sh"
    r"|docs/[\w./+-]*windows[\w./+-]*"
    r")$", re.IGNORECASE
)
WINDOWS_SOURCE_REQUIRED = {
    "repository/scripts/build-windows.sh",
    "repository/scripts/test-windows-static-relink.sh",
    "repository/third_party/px4-userland/userland/src/windows/windows_ipc.cpp",
    "repository/third_party/px4-userland/userland/src/windows/windows_sleep.cpp",
    "repository/third_party/px4-userland/userland/src/windows/windows_tuner_nonce.cpp",
    "repository/userland/tools/asicen_ts_windows.cpp",
    "repository/third_party/px4-userland/userland/tools/px4_windows_args.h",
    "repository/third_party/px4-userland/userland/tests/windows_platform_tests.cpp",
    "repository/third_party/px4-userland/userland/tests/windows_worker_tests_main.cpp",
    "repository/third_party/px4-userland/userland/include/px4/windows_tuner_nonce.h",
    "repository/userland/src/windows/libusb_stream_policy.h",
    "repository/userland/src/windows/libusb_stream_policy.cpp",
    "repository/userland/tests/windows_libusb_stream_policy_tests.cpp",
}


class AuditError(Exception):
    pass


def fail(message: str) -> None:
    raise AuditError(message)


def run(command: list[str], *, input_text: str | None = None) -> str:
    try:
        completed = subprocess.run(
            command,
            check=True,
            text=True,
            input=input_text,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        output = getattr(error, "stdout", "") or ""
        fail(f"command failed ({' '.join(command)}): {output.strip()}")
    return completed.stdout


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_version(version: str) -> None:
    if not VERSION_RE.fullmatch(version):
        fail(f"version must be strict N.N.N, got {version!r}")


def validate_platform(platform: str) -> None:
    if platform not in PLATFORMS:
        fail(f"unsupported platform: {platform}")


def _pe_u16(data: bytes, offset: int) -> int:
    if offset + 2 > len(data):
        fail("truncated PE image")
    return struct.unpack_from("<H", data, offset)[0]


def _pe_u32(data: bytes, offset: int) -> int:
    if offset + 4 > len(data):
        fail("truncated PE image")
    return struct.unpack_from("<I", data, offset)[0]


def parse_pe(data: bytes) -> dict:
    """Parse the PE headers needed for the Windows release contract.

    The parser is deliberately self-contained so the Windows audit does not
    depend on a host llvm-readobj/objdump and can run on any CI runner.
    """
    if len(data) < 0x40 or data[:2] != b"MZ":
        fail("not a PE image: missing MZ header")
    pe_offset = _pe_u32(data, 0x3C)
    if pe_offset + 24 > len(data) or data[pe_offset:pe_offset + 4] != b"PE\0\0":
        fail("not a PE image: missing PE signature")
    machine = _pe_u16(data, pe_offset + 4)
    section_count = _pe_u16(data, pe_offset + 6)
    timestamp = _pe_u32(data, pe_offset + 8)
    optional_size = _pe_u16(data, pe_offset + 20)
    characteristics = _pe_u16(data, pe_offset + 22)
    optional_offset = pe_offset + 24
    if optional_size < 0x70 or optional_offset + optional_size > len(data):
        fail("truncated PE optional header")
    magic = _pe_u16(data, optional_offset)
    dll_characteristics = _pe_u16(data, optional_offset + 70)
    number_of_directories = _pe_u32(data, optional_offset + 108) if magic == PE32_PLUS_MAGIC else 0
    sections = []
    section_offset = optional_offset + optional_size
    for index in range(section_count):
        base = section_offset + index * 40
        if base + 40 > len(data):
            fail("truncated PE section table")
        sections.append((
            _pe_u32(data, base + 12), _pe_u32(data, base + 8),
            _pe_u32(data, base + 16), _pe_u32(data, base + 20),
        ))

    def rva_to_offset(rva: int) -> int | None:
        for virtual_address, virtual_size, raw_size, raw_pointer in sections:
            span = max(virtual_size, raw_size)
            if virtual_address <= rva < virtual_address + span:
                return raw_pointer + (rva - virtual_address)
        return None

    imports = []
    exports = []
    directories = optional_offset + 112
    if magic == PE32_PLUS_MAGIC and number_of_directories >= 1:
        export_rva = _pe_u32(data, directories)
        if export_rva:
            exports = _pe_exports(data, export_rva, rva_to_offset)
    if magic == PE32_PLUS_MAGIC and number_of_directories >= 2:
        import_rva = _pe_u32(data, directories + 8)
        if import_rva:
            imports = _pe_imports(data, import_rva, rva_to_offset)
    return {
        "machine": machine, "magic": magic, "timestamp": timestamp,
        "characteristics": characteristics, "dll_characteristics": dll_characteristics,
        "imports": imports, "exports": exports,
    }


def _pe_cstring(data: bytes, offset: int) -> str:
    end = data.find(b"\0", offset)
    if end < 0:
        fail("unterminated PE string")
    try:
        return data[offset:end].decode("ascii")
    except UnicodeDecodeError:
        fail("non-ASCII PE import/export name")


def _pe_imports(data: bytes, import_rva: int, rva_to_offset) -> list[str]:
    offset = rva_to_offset(import_rva)
    if offset is None:
        fail("PE import directory RVA is not mappable")
    names: list[str] = []
    seen: set[str] = set()
    while True:
        if offset + 20 > len(data):
            fail("truncated PE import directory")
        name_rva = _pe_u32(data, offset + 12)
        if name_rva == 0:
            break
        name_offset = rva_to_offset(name_rva)
        if name_offset is None:
            fail("PE import name RVA is not mappable")
        name = _pe_cstring(data, name_offset)
        if name not in seen:
            seen.add(name)
            names.append(name)
        offset += 20
    return names


def _pe_exports(data: bytes, export_rva: int, rva_to_offset) -> list[str]:
    offset = rva_to_offset(export_rva)
    if offset is None or offset + 40 > len(data):
        fail("PE export directory is missing or truncated")
    number_of_names = _pe_u32(data, offset + 24)
    names_rva = _pe_u32(data, offset + 32)
    if number_of_names == 0 or names_rva == 0:
        return []
    names_offset = rva_to_offset(names_rva)
    if names_offset is None:
        fail("PE export name table RVA is not mappable")
    names = []
    for index in range(number_of_names):
        entry = names_offset + index * 4
        if entry + 4 > len(data):
            fail("truncated PE export name table")
        name_offset = rva_to_offset(_pe_u32(data, entry))
        if name_offset is None:
            fail("PE export name RVA is not mappable")
        names.append(_pe_cstring(data, name_offset))
    return names


def audit_windows_pe(path: Path, logical_name: str, *, is_dll: bool,
                     source_root: Path | None) -> dict:
    if path.is_symlink() or not path.is_file():
        fail(f"missing Windows artifact: {path}")
    data = path.read_bytes()
    pe = parse_pe(data)
    if pe["machine"] != IMAGE_FILE_MACHINE_AMD64:
        fail(f"{logical_name} is not an AMD64 PE image")
    if pe["magic"] != PE32_PLUS_MAGIC:
        fail(f"{logical_name} is not a PE32+ image")
    if pe["timestamp"] != 0:
        fail(f"{logical_name} does not have a zero reproducible PE timestamp")
    if bool(pe["characteristics"] & IMAGE_FILE_DLL) != is_dll:
        fail(f"{logical_name} PE DLL/EXE characteristic mismatch")
    hardened = IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE | IMAGE_DLLCHARACTERISTICS_NX_COMPAT
    if pe["dll_characteristics"] & hardened != hardened:
        fail(f"{logical_name} lacks ASLR/NX hardening")
    imports = sorted({name.lower() for name in pe["imports"]})
    for dll in imports:
        if dll == WINDOWS_LIBUSB_DLL:
            fail(f"{logical_name} imports {WINDOWS_LIBUSB_DLL}; libusb is statically linked")
        if dll in WINDOWS_SYSTEM_IMPORTS or dll.startswith(WINDOWS_UCRT_IMPORT_PREFIX):
            continue
        fail(f"{logical_name} imports unexpected DLL: {dll}")
    if logical_name == "asicend.exe":
        if WINDOWS_LIBUSB_MARKER not in data:
            fail(f"{logical_name} does not contain the static libusb version URL")
    elif logical_name in WINDOWS_PROGRAM_ARTIFACTS and WINDOWS_LIBUSB_MARKER in data:
        fail(f"{logical_name} contains the libusb version URL without linking libusb")
    if is_dll:
        exports = sorted(set(pe["exports"]))
        if not any(symbol.startswith("libusb_") for symbol in exports):
            fail(f"{logical_name} does not export the libusb ABI")
    exports = sorted(set(pe["exports"])) if is_dll else []
    if source_root is not None:
        marker = str(source_root).encode()
        if marker and marker in data:
            fail(f"{logical_name} leaks the source/build path")
    return {
        "artifact": logical_name, "format": "PE32+", "machine": "AMD64", "magic": "0x20B",
        "imports": imports, "exports": exports, "sha256": hashlib.sha256(data).hexdigest(),
    }


def safe_member(name: str) -> PurePosixPath:
    if "\\" in name or name.startswith("/"):
        fail(f"unsafe archive member path: {name!r}")
    path = PurePosixPath(name)
    if (not name or path == PurePosixPath(".") or ".." in path.parts or
            path.as_posix() != name or ":" in path.parts[0]):
        fail(f"unsafe archive member path: {name!r}")
    return path


def archive_members(archive: Path, *, forbidden: re.Pattern | None = FORBIDDEN) -> dict[str, tarfile.TarInfo]:
    if not archive.is_file():
        fail(f"archive not found: {archive}")
    members: dict[str, tarfile.TarInfo] = {}
    try:
        with tarfile.open(archive, "r:*") as stream:
            for member in stream.getmembers():
                safe_member(member.name)
                if member.name in members:
                    fail(f"duplicate archive member: {member.name}")
                if not member.isfile():
                    fail(f"archive member is not a regular file: {member.name}")
                if forbidden is not None and forbidden.search(member.name) and member.name != FIRMWARE_MEMBER:
                    fail(f"forbidden archive member: {member.name}")
                members[member.name] = member
    except (OSError, tarfile.TarError) as error:
        fail(f"cannot read archive {archive}: {error}")
    return members


def read_archive_file(archive: Path, name: str) -> bytes:
    with tarfile.open(archive, "r:*") as stream:
        member = stream.getmember(name)
        handle = stream.extractfile(member)
        if handle is None:
            fail(f"cannot read archive member: {name}")
        return handle.read()


def verify_checksums(archive: Path, members: dict[str, tarfile.TarInfo]) -> None:
    if "SHA256SUMS" not in members:
        fail("SHA256SUMS is missing")
    try:
        text = read_archive_file(archive, "SHA256SUMS").decode("ascii")
    except (UnicodeDecodeError, OSError, tarfile.TarError) as error:
        fail(f"invalid SHA256SUMS: {error}")
    listed: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            fail(f"invalid SHA256SUMS line: {line!r}")
        digest, name = match.groups()
        safe_member(name)
        if name in listed:
            fail(f"duplicate checksum entry: {name}")
        listed[name] = digest
    required = set(members) - {"SHA256SUMS"}
    if listed.keys() != required:
        fail("SHA256SUMS does not list exactly every other archive member")
    with tarfile.open(archive, "r:*") as stream:
        for name, expected in listed.items():
            handle = stream.extractfile(stream.getmember(name))
            if handle is None or hashlib.sha256(handle.read()).hexdigest() != expected:
                fail(f"checksum mismatch: {name}")


def zip_members(archive: Path) -> dict[str, zipfile.ZipInfo]:
    """Validate the Windows zip envelope and return its members by name."""
    if not archive.is_file():
        fail(f"archive not found: {archive}")
    members: dict[str, zipfile.ZipInfo] = {}
    try:
        with zipfile.ZipFile(archive) as stream:
            for info in stream.infolist():
                name = info.filename
                safe_member(name)
                pure = PurePosixPath(name)
                if ":" in pure.parts[0]:
                    fail(f"unsafe archive member path: {name!r}")
                if name in members:
                    fail(f"duplicate archive member: {name}")
                if info.is_dir():
                    fail(f"archive member is not a regular file: {name}")
                mode = (info.external_attr >> 16) & 0o170000
                if mode == stat.S_IFLNK or (mode and mode not in (stat.S_IFREG,)):
                    fail(f"archive member is not a regular file: {name}")
                if not name.isascii():
                    fail(f"archive member name is not ASCII: {name!r}")
                if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                    fail(f"unsupported archive compression for {name}")
                members[name] = info
    except (OSError, zipfile.BadZipFile, UnicodeDecodeError) as error:
        fail(f"cannot read archive {archive}: {error}")
    return members


def read_zip_file(archive: Path, name: str) -> bytes:
    try:
        with zipfile.ZipFile(archive) as stream:
            return stream.read(name)
    except (KeyError, OSError, zipfile.BadZipFile) as error:
        fail(f"cannot read archive member {name}: {error}")


def verify_windows_checksums(archive: Path, members: dict[str, zipfile.ZipInfo]) -> None:
    if "SHA256SUMS" not in members:
        fail("SHA256SUMS is missing")
    try:
        text = read_zip_file(archive, "SHA256SUMS").decode("ascii")
    except UnicodeDecodeError as error:
        fail(f"invalid SHA256SUMS: {error}")
    listed: dict[str, str] = {}
    for line in text.splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        if not match:
            fail(f"invalid SHA256SUMS line: {line!r}")
        digest, name = match.groups()
        safe_member(name)
        if name in listed:
            fail(f"duplicate checksum entry: {name}")
        listed[name] = digest
    if listed.keys() != set(members) - {"SHA256SUMS"}:
        fail("SHA256SUMS does not list exactly every other archive member")
    for name, expected in listed.items():
        if hashlib.sha256(read_zip_file(archive, name)).hexdigest() != expected:
            fail(f"checksum mismatch: {name}")


def verify_windows_manifest(archive: Path, members: dict[str, zipfile.ZipInfo], version: str) -> dict:
    try:
        payload = read_zip_file(archive, "manifest.json").decode("utf-8")
        manifest = json.loads(payload, object_pairs_hook=reject_duplicate_json_keys,
                              parse_constant=reject_json_constant)
    except (UnicodeDecodeError, OSError, zipfile.BadZipFile, ValueError) as error:
        fail(f"invalid manifest.json: {error}")
    if not isinstance(manifest, dict):
        fail("invalid manifest.json: top-level value must be an object")
    if manifest.get("schema") != 1 or manifest.get("platform") != WINDOWS_PLATFORM:
        fail("manifest schema/platform mismatch")
    if manifest.get("architecture") != "x86_64":
        fail("manifest architecture must be x86_64 for the Windows archive")
    if manifest.get("embedded_libusb") != {"version": "1.0.30", "linkage": "static"}:
        fail("manifest libusb linkage metadata mismatch")
    if "libusb_dll" in manifest:
        fail("manifest names a packaged libusb DLL")
    if not isinstance(manifest.get("source_ref"), str) or not manifest["source_ref"]:
        fail("manifest source_ref metadata is missing")
    validate_version(str(manifest.get("version", "")))
    if manifest.get("version") != version:
        fail("manifest version does not match the archive name")
    if sorted(manifest.get("programs", [])) != sorted(PROGRAMS):
        fail("manifest does not describe exactly the three production programs")
    inventory = manifest.get("files")
    if not isinstance(inventory, dict):
        fail("manifest files inventory is missing")
    expected_names = set(members) - {"manifest.json", "SHA256SUMS"}
    if set(inventory) != expected_names:
        fail("manifest files inventory does not match archived payload")
    for name in sorted(expected_names):
        record = inventory[name]
        if (not isinstance(record, dict) or set(record) != {"sha256", "size"} or
                not isinstance(record["sha256"], str) or
                not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) or
                not isinstance(record["size"], int) or isinstance(record["size"], bool) or
                record["size"] < 0):
            fail(f"invalid manifest file record: {name}")
        data = read_zip_file(archive, name)
        if record["size"] != len(data) or record["sha256"] != hashlib.sha256(data).hexdigest():
            fail(f"manifest file record mismatch: {name}")
    return manifest


def audit_windows_archive(args: argparse.Namespace) -> dict:
    archive = args.archive.resolve()
    members = zip_members(archive)
    if set(members) != WINDOWS_ARCHIVE_MEMBERS:
        fail("Windows archive member allowlist mismatch; unexpected="
             f"{sorted(set(members) - WINDOWS_ARCHIVE_MEMBERS)}, "
             f"missing={sorted(WINDOWS_ARCHIVE_MEMBERS - set(members))}")
    verify_firmware(read_zip_file(archive, FIRMWARE_MEMBER),
                    read_zip_file(archive, FIRMWARE_NOTICE_MEMBER),
                    {name: member.external_attr >> 16 & 0o777 for name, member in members.items()})
    if members["libusb/COPYING"].external_attr >> 16 & 0o777 != 0o644:
        fail("Windows archive libusb/COPYING must have mode 644")
    if hashlib.sha256(read_zip_file(archive, "libusb/COPYING")).hexdigest() != LIBUSB_COPYING_SHA256:
        fail("Windows archive libusb/COPYING does not match the verified libusb license")
    for name, expected in WINDOWS_TOOLCHAIN_LICENSE_SHA256.items():
        if members[name].external_attr >> 16 & 0o777 != 0o644:
            fail(f"Windows archive toolchain license must have mode 644: {name}")
        if hashlib.sha256(read_zip_file(archive, name)).hexdigest() != expected:
            fail(f"Windows archive toolchain license does not match the pinned text: {name}")
    try:
        notice = read_zip_file(archive, "DEPENDENCY-NOTICE.txt").decode("utf-8")
    except UnicodeDecodeError as error:
        fail(f"invalid Windows dependency notice: {error}")
    fields = notice_fields(notice)
    match = re.fullmatch(r"asicen-userland-([0-9]+\.[0-9]+\.[0-9]+)-windows-x86_64\.zip", archive.name)
    if not match:
        fail(f"Windows archive name does not match the platform/version pattern: {archive.name}")
    version = match.group(1)
    required_fields = {
        "dependency.libusb.version": "1.0.30",
        "dependency.libusb.linkage": "static",
        "dependency.libusb.license": "LGPL-2.1-or-later",
        "dependency.toolchain": "llvm-mingw-20250910-ucrt-x86_64",
        "dependency.toolchain.licenses": WINDOWS_TOOLCHAIN_LICENSE_NOTICE,
        "corresponding-source-archive": f"asicen-userland-{version}-source.tar.gz",
    }
    for key, value in required_fields.items():
        if fields.get(key) != value:
            fail(f"Windows dependency notice field mismatch: {key}={value}")
    if "dependency.libusb.dll" in fields:
        fail("Windows dependency notice still names a libusb DLL")
    source_root = args.repo_root.resolve() if getattr(args, "repo_root", None) else None
    records = {}
    with tempfile.TemporaryDirectory(prefix="asicen-windows-audit-") as temporary:
        for name in WINDOWS_PROGRAM_ARTIFACTS:
            path = Path(temporary) / name
            path.write_bytes(read_zip_file(archive, name))
            records[name] = audit_windows_pe(path, name, is_dll=False, source_root=source_root)
    for name in WINDOWS_PROGRAM_ARTIFACTS:
        if members[name].external_attr >> 16 & 0o777 not in (0o755, 0o644):
            fail(f"Windows archive program has an unexpected mode: {name}")
    try:
        evidence_payload = read_zip_file(archive, "evidence/binary-audit.json").decode("utf-8")
        evidence = json.loads(evidence_payload, object_pairs_hook=reject_duplicate_json_keys,
                              parse_constant=reject_json_constant)
    except (UnicodeDecodeError, ValueError) as error:
        fail(f"invalid Windows binary evidence: {error}")
    if not isinstance(evidence, dict) or set(evidence) != {"platform", "programs", "extra"}:
        fail("Windows binary evidence schema has unknown or missing fields")
    if evidence.get("platform") != WINDOWS_PLATFORM:
        fail("Windows binary evidence platform mismatch")
    programs = evidence.get("programs")
    if not isinstance(programs, list) or len(programs) != len(PROGRAMS):
        fail("Windows binary evidence must contain exactly three program records")
    program_names = []
    for record in programs:
        if not isinstance(record, dict) or record.get("format") != "PE32+":
            fail("invalid Windows binary evidence record")
        if record.get("artifact") not in WINDOWS_PROGRAM_ARTIFACTS:
            fail("invalid Windows binary evidence artifact name")
        program_names.append(record["artifact"])
    if set(program_names) != set(WINDOWS_PROGRAM_ARTIFACTS) or len(set(program_names)) != len(PROGRAMS):
        fail("Windows binary evidence must contain each executable exactly once")
    extra = evidence.get("extra")
    if not isinstance(extra, list) or extra:
        fail("Windows binary evidence must not contain extra artifacts")
    for record in programs:
        name = record["artifact"]
        if record.get("sha256") != records[name]["sha256"]:
            fail(f"Windows binary evidence checksum mismatch: {name}")
        if sorted(record.get("imports", [])) != records[name]["imports"]:
            fail(f"Windows binary evidence import mismatch: {name}")
    manifest = verify_windows_manifest(archive, members, version)
    verify_windows_checksums(archive, members)
    return {"archive": str(archive), "platform": WINDOWS_PLATFORM,
            "members": sorted(members), "manifest": manifest}


def synthetic_pe(*, imports: tuple[str, ...] = (), exports: tuple[str, ...] = (),
                 machine: int = IMAGE_FILE_MACHINE_AMD64, magic: int = PE32_PLUS_MAGIC,
                 timestamp: int = 0, dll: bool = False,
                 dll_characteristics: int = (IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE |
                                             IMAGE_DLLCHARACTERISTICS_NX_COMPAT),
                 poison: bytes = b"") -> bytes:
    """Build a minimal PE32+ image for offline audit self-tests only."""
    section_rva = 0x1000
    content = bytearray()

    def align4() -> None:
        while len(content) % 4:
            content.append(0)

    descriptors = len(content)
    content.extend(b"\0" * (20 * (len(imports) + 1)))
    import_name_rvas = []
    for name in imports:
        import_name_rvas.append(section_rva + len(content))
        content.extend(name.encode("ascii") + b"\0")
    for index, rva in enumerate(import_name_rvas):
        struct.pack_into("<I", content, descriptors + index * 20 + 12, rva)
    import_rva = section_rva + descriptors if imports else 0
    export_rva = 0
    if exports:
        align4()
        export_dir = len(content)
        content.extend(b"\0" * 40)
        names_table = len(content)
        content.extend(b"\0" * (4 * len(exports)))
        export_name_rvas = []
        for name in exports:
            export_name_rvas.append(section_rva + len(content))
            content.extend(name.encode("ascii") + b"\0")
        struct.pack_into("<I", content, export_dir + 24, len(exports))
        struct.pack_into("<I", content, export_dir + 32, section_rva + names_table)
        for index, rva in enumerate(export_name_rvas):
            struct.pack_into("<I", content, names_table + index * 4, rva)
        export_rva = section_rva + export_dir
    content.extend(poison)
    section_size = len(content)

    optional_size = 0xF0
    pe_offset = 0x40
    optional_offset = pe_offset + 24
    section_header_offset = optional_offset + optional_size
    raw_pointer = 0x200
    image = bytearray(raw_pointer + ((section_size + 0x1FF) & ~0x1FF))
    image[0:2] = b"MZ"
    struct.pack_into("<I", image, 0x3C, pe_offset)
    image[pe_offset:pe_offset + 4] = b"PE\0\0"
    characteristics = 0x22 | (IMAGE_FILE_DLL if dll else 0)
    struct.pack_into("<HHIIIHH", image, pe_offset + 4, machine, 1, timestamp, 0, 0,
                     optional_size, characteristics)
    struct.pack_into("<H", image, optional_offset, magic)
    struct.pack_into("<I", image, optional_offset + 16, section_rva)
    struct.pack_into("<I", image, optional_offset + 20, section_rva)
    struct.pack_into("<Q", image, optional_offset + 24, 0x140000000)
    struct.pack_into("<II", image, optional_offset + 32, 0x1000, 0x200)
    struct.pack_into("<II", image, optional_offset + 56, section_rva + ((section_size + 0xFFF) & ~0xFFF), raw_pointer)
    struct.pack_into("<H", image, optional_offset + 68, 3)
    struct.pack_into("<H", image, optional_offset + 70, dll_characteristics)
    struct.pack_into("<I", image, optional_offset + 108, 16)
    directories = optional_offset + 112
    if export_rva:
        struct.pack_into("<II", image, directories, export_rva, 40)
    if import_rva:
        struct.pack_into("<II", image, directories + 8, import_rva, 20 * (len(imports) + 1))
    image[section_header_offset:section_header_offset + 8] = b".text\0\0\0"
    struct.pack_into("<IIII", image, section_header_offset + 8, section_size, section_rva,
                     section_size, raw_pointer)
    image[raw_pointer:raw_pointer + section_size] = content
    return bytes(image)


def notice_fields(text: str) -> dict[str, str]:
    fields = {}
    for line in text.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if re.fullmatch(r"[a-z0-9.-]+", key):
            fields[key] = value
    return fields


def reject_duplicate_json_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"duplicate JSON object key: {key}")
        result[key] = value
    return result


def reject_json_constant(value: str) -> None:
    raise ValueError(f"invalid JSON constant: {value}")


def read_json_archive_file(archive: Path, name: str) -> dict:
    try:
        payload = read_archive_file(archive, name).decode("utf-8")
        value = json.loads(
            payload,
            object_pairs_hook=reject_duplicate_json_keys,
            parse_constant=reject_json_constant,
        )
    except (KeyError, UnicodeDecodeError, OSError, tarfile.TarError, ValueError) as error:
        fail(f"invalid {name}: {error}")
    if not isinstance(value, dict):
        fail(f"invalid {name}: top-level value must be an object")
    return value


def validate_string_list(value: object, field: str) -> list[str]:
    if (not isinstance(value, list) or
            any(not isinstance(item, str) for item in value) or
            len(set(value)) != len(value)):
        fail(f"invalid binary evidence {field}")
    return value


def validate_binary_evidence_record(record: object, platform: str) -> dict:
    if not isinstance(record, dict):
        fail("binary evidence artifact record must be an object")
    if platform.startswith("linux-"):
        fields = {"artifact", "format", "interpreter", "needed", "sha256", "libc", "glibc_floor"}
        if set(record) != fields or record.get("format") != "ELF":
            fail("invalid Linux binary evidence schema")
        if not isinstance(record.get("interpreter"), (str, type(None))):
            fail("invalid Linux binary evidence interpreter")
        validate_string_list(record.get("needed"), "needed")
        if record.get("libc") not in ("glibc", "musl"):
            fail("invalid Linux binary evidence libc")
        if record.get("glibc_floor") not in (None, "2.31"):
            fail("invalid Linux binary evidence glibc floor")
    elif platform == "darwin-arm64":
        fields = {"artifact", "format", "install_id", "dependencies", "dwarf_sections", "nlocalsym", "local_metadata", "sha256"}
        if set(record) != fields or record.get("format") != "Mach-O":
            fail("invalid macOS binary evidence schema")
        if not isinstance(record.get("install_id"), (str, type(None))):
            fail("invalid macOS binary evidence install_id")
        validate_string_list(record.get("dependencies"), "dependencies")
        validate_string_list(record.get("dwarf_sections"), "dwarf_sections")
        if record["dwarf_sections"]:
            fail("macOS binary evidence records DWARF sections")
        if (isinstance(record.get("nlocalsym"), bool) or
                not isinstance(record.get("nlocalsym"), int) or
                record["nlocalsym"] not in (0, 1)):
            fail("invalid macOS binary evidence local symbol count")
        local_metadata = validate_string_list(record.get("local_metadata"), "local_metadata")
        if len(local_metadata) != record["nlocalsym"] or any(
                not re.fullmatch(r"radr://[0-9]+", item) for item in local_metadata):
            fail("invalid macOS local symbol metadata")
    else:
        fields = {"artifact", "format", "interpreter", "needed", "sha256"}
        if set(record) != fields or record.get("format") != "Android ELF":
            fail("invalid Android binary evidence schema")
        if not isinstance(record.get("interpreter"), str):
            fail("invalid Android binary evidence interpreter")
        validate_string_list(record.get("needed"), "needed")
    if (not isinstance(record.get("artifact"), str) or
            not isinstance(record.get("sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        fail("invalid binary evidence artifact or sha256")
    return record


def verify_binary_evidence(archive: Path, members: dict[str, tarfile.TarInfo],
                           platform: str) -> dict:
    evidence = read_json_archive_file(archive, "evidence/binary-audit.json")
    allowed_fields = {"platform", "programs", "extra"}
    if platform.startswith("android"):
        allowed_fields.add("android")
    if set(evidence) != allowed_fields:
        fail("binary evidence schema has unknown or missing fields")
    if evidence.get("platform") != platform:
        fail("binary evidence platform mismatch")

    programs = evidence.get("programs")
    if not isinstance(programs, list) or len(programs) != len(PROGRAMS):
        fail("binary evidence programs must contain exactly three records")
    program_records = [validate_binary_evidence_record(record, platform) for record in programs]
    program_names = [record["artifact"] for record in program_records]
    if set(program_names) != set(PROGRAMS) or len(set(program_names)) != len(PROGRAMS):
        fail("binary evidence programs must contain asicend, asicen-ts, and asicenctl exactly once")

    expected_extra = {
        "linux-glibc-x86_64": "ifd/asicen-userland-ifd.so",
        "linux-glibc-aarch64": "ifd/asicen-userland-ifd.so",
        "linux-musl-x86_64": "ifd/asicen-userland-ifd.so",
        "linux-musl-aarch64": "ifd/asicen-userland-ifd.so",
        "darwin-arm64": DARWIN_IFD_ARTIFACT,
    }.get(platform)
    extra = evidence.get("extra")
    if not isinstance(extra, list):
        fail("binary evidence extra must be a list")
    if expected_extra is None:
        if extra:
            fail("Android binary evidence must not contain extra artifacts")
        records = program_records
    else:
        if len(extra) != 1:
            fail("native binary evidence must contain exactly one extra artifact")
        extra_record = validate_binary_evidence_record(extra[0], platform)
        if extra_record["artifact"] != expected_extra:
            fail("binary evidence extra artifact does not match platform")
        records = program_records + [extra_record]

    expected_libc = {
        "linux-glibc-x86_64": "glibc", "linux-glibc-aarch64": "glibc",
        "linux-musl-x86_64": "musl", "linux-musl-aarch64": "musl",
    }.get(platform)
    expected_interpreter = {
        "android-aarch64": "/system/bin/linker64",
        "android-armv7a": "/system/bin/linker",
        "android-x86_64": "/system/bin/linker64",
    }.get(platform)
    for record in records:
        artifact = record["artifact"]
        if artifact not in members:
            fail(f"binary evidence artifact is not archived: {artifact}")
        if platform.startswith("linux-"):
            expected_record_libc = expected_libc if artifact not in PROGRAMS else "musl"
            if record["interpreter"] is not None or record["libc"] != expected_record_libc:
                fail(f"Linux binary evidence static/libc mismatch: {artifact}")
            if artifact not in PROGRAMS and expected_libc == "glibc" and record["glibc_floor"] != "2.31":
                fail(f"glibc Linux binary evidence floor mismatch: {artifact}")
        elif platform.startswith("android") and record["interpreter"] != expected_interpreter:
            fail(f"Android binary evidence interpreter mismatch: {artifact}")
        actual = hashlib.sha256(read_archive_file(archive, artifact)).hexdigest()
        if record["sha256"] != actual:
            fail(f"binary evidence checksum mismatch: {artifact}")

    if platform.startswith("android"):
        android = evidence.get("android")
        if (not isinstance(android, dict) or set(android) != {"ndk_revision", "inventory_members"} or
                not isinstance(android.get("ndk_revision"), str)):
            fail("invalid Android binary evidence metadata schema")
        inventory_members = validate_string_list(android.get("inventory_members"), "inventory_members")
        if tuple(inventory_members) != ANDROID_INVENTORY_MEMBERS:
            fail("Android binary evidence inventory_members do not match the allowlist")
        if any(name not in members for name in inventory_members):
            fail("Android binary evidence inventory member is not archived")
    return evidence


def verify_manifest(archive: Path, members: dict[str, tarfile.TarInfo], platform: str) -> dict:
    manifest = read_json_archive_file(archive, "manifest.json")
    if manifest.get("schema") != 1 or manifest.get("platform") != platform:
        fail("manifest schema/platform mismatch")
    expected_libc = ("glibc" if platform.startswith("linux-glibc-") else
                     "musl" if platform.startswith("linux-musl-") else
                     "android" if platform.startswith("android-") else "darwin")
    if manifest.get("libc") != expected_libc or manifest.get("architecture") != platform.rsplit("-", 1)[-1]:
        fail("manifest libc/architecture metadata mismatch")
    if not isinstance(manifest.get("source_ref"), str) or not manifest["source_ref"]:
        fail("manifest source_ref metadata is missing")
    if manifest.get("embedded_libusb") != {"version": "1.0.30", "linkage": "static"}:
        fail("manifest embedded libusb metadata mismatch")
    validate_version(str(manifest.get("version", "")))
    if sorted(manifest.get("programs", [])) != sorted(PROGRAMS):
        fail("manifest does not describe exactly the three production programs")
    inventory = manifest.get("files")
    if not isinstance(inventory, dict):
        fail("manifest files inventory is missing")
    expected_names = set(members) - {"manifest.json", "SHA256SUMS"}
    if set(inventory) != expected_names:
        fail("manifest files inventory does not match archived payload")
    with tarfile.open(archive, "r:*") as stream:
        for name in sorted(expected_names):
            record = inventory[name]
            if (not isinstance(record, dict) or set(record) != {"sha256", "size"} or
                    not isinstance(record["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) or
                    not isinstance(record["size"], int) or isinstance(record["size"], bool) or record["size"] < 0):
                fail(f"invalid manifest file record: {name}")
            payload = stream.extractfile(stream.getmember(name))
            if payload is None:
                fail(f"cannot read manifest file: {name}")
            data = payload.read()
            if record["size"] != len(data) or record["sha256"] != hashlib.sha256(data).hexdigest():
                fail(f"manifest file record mismatch: {name}")
    return manifest


def parse_needed(dynamic: str) -> set[str]:
    return set(re.findall(r"Shared library: \[([^]]+)\]", dynamic))


def validate_reader_template(data: bytes) -> None:
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        fail("reader configuration is not UTF-8")
    placeholders = set(re.findall(r"@[A-Z][A-Z0-9_]*@", text))
    if placeholders != READER_PLACEHOLDERS:
        fail(f"reader configuration placeholder mismatch: {sorted(placeholders)}")
    if text.count("@ASICEN_ACCESS@") != 1 or "access=@ASICEN_ACCESS@" not in text:
        fail("reader configuration must expose exactly one ASICEN access placeholder")


def readelf_path() -> str:
    for candidate in ("readelf", "llvm-readelf"):
        found = next((part for part in os.get_exec_path() if Path(part, candidate).is_file()), None)
        if found:
            return str(Path(found, candidate))
    fail("readelf or llvm-readelf is required")


def audit_elf_sections(path: Path, readelf: str, *, reject_build_id: bool) -> None:
    sections = run([readelf, "-SW", str(path)])
    names = re.findall(r"^\s*\[\s*\d+\]\s+(\S+)", sections, re.MULTILINE)
    if reject_build_id and ".note.gnu.build-id" in names:
        fail(f"GNU Build ID section is forbidden in release ELF: {path}")
    forbidden = [name for name in names if name.startswith((".debug", ".zdebug")) or
                 name in (".symtab", ".symtab_shndx")]
    if forbidden:
        fail(f"debug or regular symbol sections are forbidden: {path}: {forbidden}")


def audit_linux(path: Path, logical_name: str, *, platform: str, shared: bool, require_libusb: bool,
                reject_build_id: bool, reject_pcsc: bool = False) -> dict:
    target = LINUX_TARGETS[platform]
    readelf = readelf_path()
    audit_elf_sections(path, readelf, reject_build_id=reject_build_id)
    header = run([readelf, "-h", str(path)])
    program = run([readelf, "-lW", str(path)])
    dynamic = run([readelf, "-d", str(path)])
    if "ELF" not in header:
        fail(f"not an ELF file: {path}")
    machine = re.search(r"^\s*Machine:\s*(.+)$", header, re.MULTILINE)
    if not machine or machine.group(1).strip() != target["machine"]:
        fail(f"wrong Linux ELF machine for {platform}: {path}")
    interpreter = re.search(r"Requesting program interpreter: ([^]]+)", program)
    if not shared and interpreter:
        fail(f"static Linux executable has PT_INTERP: {path}")
    if shared and interpreter:
        fail(f"Linux IFD shared object has PT_INTERP: {path}")
    needed = parse_needed(dynamic)
    if not shared and needed:
        fail(f"static Linux executable has DT_NEEDED: {path}: {sorted(needed)}")
    if shared:
        if target["needed"] is None:
            if needed != {target["libc"]}:
                fail(f"musl IFD must only require matching libc: {path}: {sorted(needed)}")
        elif target["libc"] not in needed or not needed <= target["needed"]:
            fail(f"glibc IFD has unexpected host ABI dependency: {path}: {sorted(needed)}")
        output = run([nm_path(), "-D", "--defined-only", str(path)])
        symbols = {line.split()[-1] for line in output.splitlines() if line.split()}
        if symbols != set(IFD_EXPORTS):
            fail(f"Linux IFD export set mismatch: {path}: {sorted(symbols)}")
    elif require_libusb:
        # Static libusb is intentionally present in the executable, so it must
        # not appear as a dynamic dependency.
        if "libusb-1.0.so.0" in needed:
            fail(f"static executable has shared libusb dependency: {path}")
    if reject_pcsc and any("pcsc" in library.lower() for library in needed):
        fail(f"unexpected direct PC/SC client dependency: {path}")
    if "RPATH" in dynamic or "RUNPATH" in dynamic:
        fail(f"RPATH/RUNPATH is forbidden: {path}")
    versions = run([readelf, "--version-info", str(path)])
    glibc_versions = [tuple(int(part) for part in value.split("."))
                      for value in re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)+)", versions)]
    if not shared and glibc_versions:
        fail(f"static Linux executable has GLIBC version requirements: {path}")
    if shared and target["floor"] and glibc_versions and max(glibc_versions) > tuple(int(p) for p in target["floor"].split(".")):
        fail(f"glibc IFD exceeds floor {target['floor']}: {path}")
    return {"artifact": logical_name, "format": "ELF",
            "interpreter": None, "needed": sorted(needed),
            "libc": ("musl" if not shared else
                     "glibc" if target["libc"] == "libc.so.6" else "musl"),
            "glibc_floor": target["floor"] if shared else None}


def audit_macho_load_commands(path: Path) -> tuple[list[str], int]:
    load_commands = run(["otool", "-l", str(path)])
    if re.search(r"^\s*cmd\s+LC_RPATH\s*$", load_commands, re.MULTILINE):
        fail(f"LC_RPATH is forbidden: {path}")
    if re.search(r"^\s*segname\s+__DWARF\s*$", load_commands, re.MULTILINE):
        fail(f"DWARF segment is forbidden: {path}")
    dwarf_sections = re.findall(r"^\s*sectname\s+__(?:debug|zdebug)[^\s]*\s*$",
                                load_commands, re.MULTILINE)
    if dwarf_sections:
        fail(f"DWARF sections are forbidden: {path}: {dwarf_sections}")
    dysymtab_commands = re.findall(r"^\s*cmd\s+LC_DYSYMTAB\s*$", load_commands, re.MULTILINE)
    if len(dysymtab_commands) != 1:
        fail(f"Mach-O must contain exactly one LC_DYSYMTAB: {path}: {len(dysymtab_commands)}")
    dysymtab = re.search(r"^\s*cmd\s+LC_DYSYMTAB\s*$.*?(?=^\s*cmd\s+\S|\Z)",
                         load_commands, re.MULTILINE | re.DOTALL)
    if not dysymtab:
        fail(f"LC_DYSYMTAB is missing: {path}")
    local_symbols = re.search(r"^\s*nlocalsym\s+(\d+)\s*$", dysymtab.group(0), re.MULTILINE)
    if not local_symbols:
        fail(f"LC_DYSYMTAB nlocalsym is missing: {path}")
    nlocalsym = int(local_symbols.group(1))
    if nlocalsym > 1:
        fail(f"too many local Mach-O symbols: {path}: nlocalsym={nlocalsym}")
    return dwarf_sections, nlocalsym


def audit_macho_local_metadata(path: Path) -> list[str]:
    output = run([nm_path(), "-ap", str(path)])
    metadata = []
    for line in output.splitlines():
        fields = line.split()
        if not fields:
            continue
        radr_fields = [field for field in fields if field.startswith("radr://")]
        if radr_fields:
            if (len(fields) < 2 or fields[-2] != "OPT" or
                    not re.fullmatch(r"radr://[0-9]+", fields[-1]) or
                    len(radr_fields) != 1 or fields[1] != "-"):
                fail(f"unexpected Mach-O local metadata: {path}: {line}")
            metadata.append(fields[-1])
            continue
        symbol_type = fields[1] if len(fields) > 1 and len(fields[0]) > 1 else fields[0]
        if symbol_type == "-" or symbol_type.islower():
            fail(f"unexpected local Mach-O symbol: {path}: {line}")
    if len(metadata) != 1:
        fail(f"expected exactly one radr:// N_OPT metadata symbol: {path}: {metadata}")
    return metadata


def nm_path() -> str:
    for candidate in ("nm", "llvm-nm"):
        found = next((part for part in os.get_exec_path() if Path(part, candidate).is_file()), None)
        if found:
            return str(Path(found, candidate))
    fail("nm or llvm-nm is required for the IFD audit")


def audit_macho_ifd_exports(path: Path) -> None:
    output = run([nm_path(), "-gU", str(path)])
    symbols = {line.split()[-1].lstrip("_") for line in output.splitlines() if line.split()}
    expected = set(IFD_EXPORTS)
    if symbols != expected:
        fail(f"macOS IFD export set mismatch: {path}: {sorted(symbols)}")


DARWIN_SYSTEM_PREFIXES = ("/usr/lib/", "/System/Library/")


def audit_darwin(path: Path, logical_name: str, *, reject_pcsc: bool = False) -> dict:
    if run(["lipo", "-archs", str(path)]).strip() != "arm64":
        fail(f"macOS binary must contain exactly the arm64 architecture: {path}")
    dwarf_sections, nlocalsym = audit_macho_load_commands(path)
    local_metadata = audit_macho_local_metadata(path) if nlocalsym == 1 else []
    if logical_name == DARWIN_IFD_ARTIFACT:
        audit_macho_ifd_exports(path)
    output = run(["otool", "-L", str(path)])
    own_id = None
    if logical_name.endswith(".dylib"):
        install_output = run(["otool", "-D", str(path)])
        install_lines = [line.strip() for line in install_output.splitlines() if line.strip()]
        own_id = install_lines[1] if len(install_lines) > 1 else None
        expected_id = "@rpath/libasicen-userland-ifd.dylib"
        if own_id != expected_id:
            fail(f"unexpected macOS IFD install ID: {path}: {own_id!r}")
    lines = [line.strip() for line in output.splitlines()[1:] if line.strip()]
    first_name = lines[0].split(" (", 1)[0] if lines else None
    if own_id is None and logical_name.endswith(".dylib") and first_name:
        if PurePosixPath(first_name).name == PurePosixPath(logical_name).name:
            own_id = first_name
    if own_id and lines and first_name == own_id:
        lines = lines[1:]
    dependencies = [line.split(" (", 1)[0] for line in lines]
    logical_dependencies = sorted({PurePosixPath(dependency).name for dependency in dependencies})
    # libusb is statically linked into asicend; no macOS artifact may load it.
    if any("libusb-1.0" in dependency for dependency in logical_dependencies):
        fail(f"unexpected direct dynamic libusb dependency: {path}")
    if reject_pcsc and any("pcsc" in dependency.lower() for dependency in logical_dependencies):
        fail(f"unexpected direct PC/SC client dependency: {path}")
    if any(dependency.startswith("@loader_path/") or dependency.startswith("@rpath/")
           for dependency in dependencies):
        fail(f"bundled/rpath dependency is forbidden: {path}")
    host_dependencies = [dependency for dependency in dependencies
                         if not dependency.startswith(DARWIN_SYSTEM_PREFIXES)]
    if host_dependencies:
        fail(f"non-system macOS dependency is forbidden: {path}: {host_dependencies}")
    stable_install_id = None if own_id is None else (own_id if own_id.startswith("@") else PurePosixPath(own_id).name)
    return {"artifact": logical_name, "format": "Mach-O", "install_id": stable_install_id,
            "dependencies": logical_dependencies, "dwarf_sections": dwarf_sections,
            "nlocalsym": nlocalsym, "local_metadata": local_metadata}


def audit_archive_elf_sections(archive: Path, members: dict[str, tarfile.TarInfo],
                               platform: str) -> None:
    # Darwin DWARF inspection is performed by audit_binaries on macOS. The
    # resulting dwarf_sections/local-symbol fields are checked against each
    # archived binary's SHA-256 below, so this cross-platform archive audit
    # must not attempt to run the Darwin-only otool.
    if not platform.startswith(("linux-", "android-")):
        return
    # Only asicend has the Build ID reproducibility contract. The other two
    # production programs and the Linux IFD intentionally keep their baseline
    # metadata, including any GNU Build ID.
    binary_names = [(program, program == "asicend") for program in PROGRAMS]
    if platform.startswith("linux-"):
        binary_names.append(("ifd/asicen-userland-ifd.so", False))
    with tempfile.TemporaryDirectory(prefix="asicen-archive-binary-audit-") as temporary:
        root = Path(temporary)
        for name, reject_build_id in binary_names:
            if name not in members:
                fail(f"binary audit member is missing: {name}")
            path = root / PurePosixPath(name)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(read_archive_file(archive, name))
            if platform.startswith("linux-"):
                audit_linux(path, name, platform=platform, shared=name not in PROGRAMS,
                             require_libusb=name == "asicend", reject_build_id=reject_build_id,
                             reject_pcsc=name not in PROGRAMS)
            else:
                audit_elf_sections(path, readelf_path(), reject_build_id=reject_build_id)


def audit_termux_launcher(payload: bytes, mode: int) -> None:
    if mode != 0o755:
        fail(f"Termux launcher must have mode 0755, got {mode:o}")
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError:
        fail("Termux launcher is not UTF-8 text")
    if not text.startswith("#!/data/data/com.termux/files/usr/bin/sh\n"):
        fail("Termux launcher must use the Termux shell shebang")
    if "# SPDX-License-Identifier: GPL-2.0-only" not in text.splitlines()[:3]:
        fail("Termux launcher is missing its SPDX license identifier")
    if re.search(r"(^|[^A-Za-z0-9_])eval([^A-Za-z0-9_]|$)", text):
        fail("Termux launcher must not use shell eval")
    for required in ("command -v termux-usb", "command -v setsid",
                     "setsid termux-usb"):
        if required not in text:
            fail(f"Termux launcher is missing required operation: {required}")
    run(["sh", "-n"], input_text=text)


def verify_darwin_bundle_plist(payload: bytes) -> None:
    try:
        info = plistlib.loads(payload)
    except (ValueError, plistlib.InvalidFileException) as error:
        fail(f"invalid macOS IFD Info.plist: {error}")
    if not isinstance(info, dict):
        fail("macOS IFD Info.plist must be a dictionary")
    expected = {
        "CFBundleIdentifier": "io.github.khronos31.asicen-userland.ifd",
        "CFBundleExecutable": "libasicen-userland-ifd.dylib",
        "CFBundlePackageType": "BNDL",
    }
    for key, value in expected.items():
        if info.get(key) != value:
            fail(f"macOS IFD Info.plist {key} mismatch")
    if any(key in info for key in ("ifdVendorID", "ifdProductID", "IOKitPersonalities")):
        fail("macOS IFD bundle must not register a USB driver")


def audit_android(path: Path, logical_name: str, expected_interpreter: str,
                  expected_arch: str, root: Path, *, reject_build_id: bool) -> dict:
    verifier = root / "scripts" / "verify-android-elf.sh"
    if not verifier.is_file():
        fail(f"missing existing Android ELF verifier: {verifier}")
    run([str(verifier), str(path), expected_interpreter, expected_arch])
    readelf = readelf_path()
    audit_elf_sections(path, readelf, reject_build_id=reject_build_id)
    dynamic = run([readelf, "-d", str(path)])
    return {"artifact": logical_name, "format": "Android ELF",
            "interpreter": expected_interpreter, "needed": sorted(parse_needed(dynamic))}


def audit_ifd_proxy(plugin: Path, daemon_binary: Path) -> int:
    """Prove an offline IFD IPC round trip and bounded mock-daemon cleanup."""
    with tempfile.TemporaryDirectory(prefix="aifd-", dir="/tmp") as temporary:
        root = Path(temporary)
        runtime = root / "r"
        runtime.mkdir(mode=0o700)
        instance = "ifdsmoke"
        daemon_endpoint = runtime / "asicen-userland" / instance / "control.sock"
        proxy_runtime = root / "p"
        proxy_instance = "proxy"
        proxy_endpoint = proxy_runtime / "asicen-userland" / proxy_instance / "control.sock"
        proxy_runtime.mkdir(mode=0o700)
        (proxy_runtime / "asicen-userland").mkdir(mode=0o700)
        proxy_endpoint.parent.mkdir(parents=True, mode=0o700)
        for directory in (proxy_runtime, proxy_runtime / "asicen-userland",
                          proxy_endpoint.parent):
            directory.chmod(0o700)
        byte_counts = {"to-daemon": 0, "from-daemon": 0}
        counts_lock = threading.Lock()
        connection_lock = threading.Lock()
        relay_sockets: list[socket.socket] = []
        stop_proxy = threading.Event()

        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(proxy_endpoint))
        proxy_endpoint.chmod(0o600)
        listener.listen(4)
        listener.settimeout(0.1)

        def relay(client: socket.socket) -> None:
            backend = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                backend.connect(str(daemon_endpoint))
                while True:
                    readable, _, exceptional = select.select([client, backend], [],
                                                               [client, backend], 1.0)
                    if exceptional:
                        return
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            return
                        destination = backend if source is client else client
                        destination.sendall(data)
                        key = "to-daemon" if source is client else "from-daemon"
                        with counts_lock:
                            byte_counts[key] += len(data)
            except OSError:
                return
            finally:
                client.close()
                backend.close()

        def accept_clients() -> None:
            while not stop_proxy.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                with connection_lock:
                    relay_sockets.append(client)
                threading.Thread(target=relay, args=(client,), daemon=True).start()

        proxy_thread = threading.Thread(target=accept_clients, daemon=True)
        proxy_thread.start()
        with (root / "daemon.log").open("wb") as log:
            daemon = subprocess.Popen(
                [str(daemon_binary), "--runtime-dir", str(runtime),
                 "--instance", instance], stdout=subprocess.DEVNULL, stderr=log)
            try:
                deadline = time.monotonic() + 3
                while not daemon_endpoint.exists() and time.monotonic() < deadline:
                    if daemon.poll() is not None:
                        raise AssertionError(f"mock daemon exited: {daemon.returncode}")
                    time.sleep(0.01)
                if not daemon_endpoint.exists():
                    raise AssertionError("mock daemon endpoint did not appear")

                ifd = ctypes.CDLL(str(plugin), mode=getattr(os, "RTLD_NOW", 2))
                create = ifd.IFDHCreateChannelByName
                create.argtypes = [ctypes.c_uint32, ctypes.c_char_p]
                create.restype = ctypes.c_int
                presence = ifd.IFDHICCPresence
                presence.argtypes = [ctypes.c_uint32]
                presence.restype = ctypes.c_int
                close = ifd.IFDHCloseChannel
                close.argtypes = [ctypes.c_uint32]
                close.restype = ctypes.c_int

                device = (f"asicen-userland:runtime={proxy_runtime}:instance={proxy_instance}:"
                          "access=user").encode("utf-8")
                if create(0, device) != 0:
                    raise AssertionError("bundled IFD rejected the ASICEN endpoint")
                # The mock profile intentionally has no card implementation. The
                # presence result is not used as evidence of connectivity: the
                # proxy must observe an actual request and response from the daemon.
                if presence(0) != 616:
                    raise AssertionError("bundled IFD returned an unexpected mock presence")
                if close(0) != 0:
                    raise AssertionError("bundled IFD failed to close its mock channel")
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    with counts_lock:
                        observed = dict(byte_counts)
                    if observed["to-daemon"] > 0 and observed["from-daemon"] > 0:
                        break
                    time.sleep(0.01)
                if observed["to-daemon"] == 0 or observed["from-daemon"] == 0:
                    raise AssertionError(f"IFD IPC proxy saw no round trip: {observed}")
            finally:
                stop_proxy.set()
                listener.close()
                proxy_thread.join(timeout=1)
                with connection_lock:
                    active_sockets = list(relay_sockets)
                for relay_socket in active_sockets:
                    try:
                        relay_socket.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    relay_socket.close()
                if daemon.poll() is None:
                    daemon.send_signal(signal.SIGTERM)
                    try:
                        daemon.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()
                # Only surface a shutdown defect when the body did not already
                # raise, so the original failure (e.g. IFD rejection) is kept.
                if daemon.returncode != 0 and sys.exc_info()[0] is None:
                    raise AssertionError(f"mock daemon shutdown returned {daemon.returncode}")
    return 0



def audit_binaries(args: argparse.Namespace) -> dict:
    validate_platform(args.platform)
    if args.build_dir.is_symlink() or not args.build_dir.is_dir():
        fail(f"build input must be a real directory: {args.build_dir}")
    build = args.build_dir.resolve()
    if not build.is_dir():
        fail(f"build directory not found: {build}")
    result: dict = {"platform": args.platform, "programs": [], "extra": []}
    for program in PROGRAMS:
        if args.platform == WINDOWS_PLATFORM:
            path = build / f"{program}.exe"
        else:
            path = build / f"{program}{('-' + args.binary_suffix) if args.binary_suffix else ''}"
        if not path.is_file() or path.is_symlink():
            fail(f"missing production program: {path}")
        if args.platform == WINDOWS_PLATFORM:
            evidence = audit_windows_pe(path, f"{program}.exe", is_dll=False,
                                        source_root=args.repo_root.resolve())
        elif args.platform.startswith("android"):
            expected, architecture = {
                "android-aarch64": ("/system/bin/linker64", "aarch64"),
                "android-armv7a": ("/system/bin/linker", "armv7a"),
                "android-x86_64": ("/system/bin/linker64", "x86_64"),
            }[args.platform]
            evidence = audit_android(path, program, expected, architecture, args.repo_root.resolve(),
                                     reject_build_id=program == "asicend")
        elif args.platform.startswith("linux-"):
            evidence = audit_linux(path, program, platform=args.platform, shared=False,
                                   require_libusb=program == "asicend", reject_build_id=program == "asicend")
        else:
            evidence = audit_darwin(path, program)
        evidence["sha256"] = sha256(path)
        result["programs"].append(evidence)

    if args.platform.startswith("linux-"):
        metadata_path = build / "build-metadata.json"
        if metadata_path.is_symlink() or not metadata_path.is_file():
            fail("Linux static build-metadata.json is missing or unsafe")
        static_metadata = verify_linux_build_metadata(
            metadata_path.read_bytes(),
            {record["artifact"]: record["sha256"] for record in result["programs"]}, args.platform)
        if not args.ifd_library:
            fail("--ifd-library is required for Linux binary audit")
        if args.ifd_library.is_symlink():
            fail(f"Linux IFD library must be an ordinary file: {args.ifd_library}")
        path = args.ifd_library.resolve()
        if not path.is_file() or path.is_symlink():
            fail(f"missing Linux IFD library: {path}")
        evidence = audit_linux(path, "ifd/asicen-userland-ifd.so", platform=args.platform, shared=True,
                               require_libusb=False, reject_build_id=False, reject_pcsc=True)
        evidence["sha256"] = sha256(path)
        ifd_metadata_path = path.parent / "build-metadata.json"
        if ifd_metadata_path.is_symlink() or not ifd_metadata_path.is_file():
            fail("Linux IFD build-metadata.json is missing or unsafe")
        verify_linux_ifd_metadata(ifd_metadata_path.read_bytes(), evidence["sha256"], args.platform,
                                   static_metadata.get("source_ref"))
        result["extra"].append(evidence)
    elif args.platform == "darwin-arm64":
        if not args.ifd_bundle:
            fail("--ifd-bundle is required for macOS binary audit")
        path = args.ifd_bundle.resolve() / "Contents" / "MacOS" / "libasicen-userland-ifd.dylib"
        if not path.is_file() or path.is_symlink():
            fail(f"missing macOS IFD bundle executable: {path}")
        plist_path = args.ifd_bundle.resolve() / "Contents" / "Info.plist"
        if plist_path.is_symlink() or not plist_path.is_file():
            fail(f"missing macOS IFD bundle Info.plist: {plist_path}")
        verify_darwin_bundle_plist(plist_path.read_bytes())
        evidence = audit_darwin(
            path, "ifd/asicen-userland-ifd.bundle/Contents/MacOS/libasicen-userland-ifd.dylib",
            reject_pcsc=True)
        evidence["sha256"] = sha256(path)
        result["extra"].append(evidence)

    if args.platform.startswith("android"):
        if not args.link_map_dir or not args.ndk_root:
            fail("Android binary audit requires explicit --link-map-dir and --ndk-root")
        link_maps = args.link_map_dir.resolve()
        ndk = args.ndk_root.resolve()
        properties = ndk / "source.properties"
        notice = ndk / "NOTICE"
        toolchain_notice = ndk / "NOTICE.toolchain"
        for required in (properties, notice, toolchain_notice):
            if not required.is_file():
                fail(f"required NDK file is absent: {required}")
        revision_line = next((line for line in properties.read_text().splitlines()
                              if line.startswith("Pkg.Revision = ")), "")
        revision = revision_line.split("=", 1)[1].strip() if revision_line else ""
        if not revision.startswith("27."):
            fail(f"NDK r27 is required, got {revision or 'unknown'}")
        inventory_dir = args.inventory_dir.resolve() if args.inventory_dir else None
        if inventory_dir:
            inventory_dir.mkdir(parents=True, exist_ok=True)
        inventories = []
        inventory_tool = args.repo_root.resolve() / "scripts" / "android-link-inventory.py"
        for program in PROGRAMS:
            link_map = link_maps / f"{program}.map"
            if not link_map.is_file() or link_map.is_symlink():
                fail(f"missing explicit Android link map: {link_map}")
            if inventory_dir is None:
                fail("--inventory-dir is required for Android binary audit")
            inventory = inventory_dir / f"{program}-static-archives.tsv"
            command = [sys.executable, str(inventory_tool), "--map", str(link_map), "--output", str(inventory)]
            if program == "asicend":
                command.append("--require-libusb")
            run(command)
            text = inventory.read_text(encoding="utf-8")
            if "ndk-runtime\tlibc++_static.a\t" not in text:
                fail(f"NDK libc++ member inventory is missing for {program}")
            if program == "asicend" and "libusb\tlibusb-1.0.a\t" not in text:
                fail("static libusb member inventory is missing for asicend")
            inventories.append(f"evidence/inventory/{program}-static-archives.tsv")
        result["android"] = {"ndk_revision": revision, "inventory_members": inventories}
    return result


def verify_linux_mdev_modes(modes: dict[str, int], platform: str) -> None:
    if not platform.startswith("linux-"):
        return
    expected_modes = {
        "mdev/asicen-userland-mdev.conf": 0o644,
        "mdev/asicen-userland-mdev.sh": 0o755,
        "mdev/asicen-userland-mdev.start": 0o755,
    }
    for name, expected_mode in expected_modes.items():
        actual_mode = modes.get(name)
        if actual_mode != expected_mode:
            fail(f"Linux mdev file has mode {actual_mode!r}, expected {expected_mode:o}: {name}")


def verify_firmware(payload: bytes, notice: bytes, modes: dict[str, int]) -> None:
    if len(payload) != FIRMWARE_SIZE or hashlib.sha256(payload).hexdigest() != FIRMWARE_SHA256:
        fail("binary archive firmware does not match the recorded vendor component")
    if notice != FIRMWARE_NOTICE.encode("utf-8"):
        fail("vendor firmware notice does not preserve exact provenance and unresolved rights status")
    for name in (FIRMWARE_MEMBER, FIRMWARE_NOTICE_MEMBER):
        if modes.get(name) != 0o644:
            fail(f"vendor firmware material must have mode 644: {name}")


def verify_linux_build_metadata(payload: bytes, hashes: dict[str, str], platform: str) -> dict:
    try:
        metadata = json.loads(payload.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys,
                              parse_constant=reject_json_constant)
    except (UnicodeDecodeError, ValueError) as error:
        fail(f"invalid Linux static build metadata: {error}")
    expected = {"schema", "compiler_target", "libc_archive_sha256", "libc_identification",
                "production_binary_sha256"}
    if (not isinstance(metadata, dict) or set(metadata) - {"source_ref"} != expected or
            metadata.get("schema") != 1):
        fail("invalid Linux static build metadata schema")
    if "source_ref" in metadata and (not isinstance(metadata["source_ref"], str) or not metadata["source_ref"]):
        fail("invalid Linux static build metadata source_ref")
    architecture = platform.rsplit("-", 1)[-1]
    target = metadata.get("compiler_target")
    if (not isinstance(target, str) or
            not re.fullmatch(rf"{re.escape(architecture)}-[A-Za-z0-9_.+-]*linux-musl[A-Za-z0-9_.+-]*", target)):
        fail("Linux static build metadata does not identify a matching musl compiler target")
    identification = metadata.get("libc_identification")
    if (not isinstance(identification, str) or "musl" not in identification.lower() or
            not re.search(r"\b1\.2\.5\b", identification)):
        fail("Linux static build metadata does not identify musl 1.2.5")
    if (not isinstance(metadata.get("libc_archive_sha256"), str) or
            not re.fullmatch(r"[0-9a-f]{64}", metadata["libc_archive_sha256"])):
        fail("Linux static build metadata lacks its libc archive SHA-256")
    if metadata.get("production_binary_sha256") != hashes or set(hashes) != set(PROGRAMS):
        fail("Linux static build metadata does not match the production binary hashes")
    return metadata


def verify_linux_ifd_metadata(payload: bytes, library_sha256: str, platform: str,
                              source_ref: str | None) -> dict:
    try:
        metadata = json.loads(payload.decode("utf-8"), object_pairs_hook=reject_duplicate_json_keys,
                              parse_constant=reject_json_constant)
    except (UnicodeDecodeError, ValueError) as error:
        fail(f"invalid Linux IFD build metadata: {error}")
    if (not isinstance(metadata, dict) or
            set(metadata) != {"schema", "source_ref", "compiler_target", "library_sha256"} or
            metadata.get("schema") != 1):
        fail("invalid Linux IFD build metadata schema")
    if not isinstance(metadata.get("source_ref"), str) or not metadata["source_ref"]:
        fail("Linux IFD build metadata source_ref is missing")
    if source_ref is not None and metadata["source_ref"] != source_ref:
        fail("Linux commands and IFD do not identify the same source_ref")
    architecture = platform.rsplit("-", 1)[-1]
    libc = "musl" if platform.startswith("linux-musl-") else "gnu"
    target = metadata.get("compiler_target")
    if (not isinstance(target, str) or
            not re.fullmatch(rf"{re.escape(architecture)}-[A-Za-z0-9_.+-]*linux-{libc}[A-Za-z0-9_.+-]*", target)):
        fail("Linux IFD compiler target does not match the platform architecture/libc")
    if metadata.get("library_sha256") != library_sha256:
        fail("Linux IFD build metadata does not match the library hash")
    return metadata


def expected_binary_members(platform: str) -> set[str]:
    validate_platform(platform)
    if platform == WINDOWS_PLATFORM:
        return set(WINDOWS_ARCHIVE_MEMBERS)
    expected = set(COMMON) | set(PROGRAMS)
    if platform.startswith("linux-"):
        expected |= {"ifd/asicen-userland-ifd.so", "reader.conf.d/asicen-userland.conf"} | LINUX_MDEV
        expected |= set(LINUX_RUNTIME_LICENSE_SHA256)
        expected.add(LINUX_BUILD_METADATA_MEMBER)
        expected.add(LINUX_IFD_METADATA_MEMBER)
    elif platform == "darwin-arm64":
        expected |= {
            "ifd/asicen-userland-ifd.bundle/Contents/Info.plist",
            "ifd/asicen-userland-ifd.bundle/Contents/MacOS/libasicen-userland-ifd.dylib",
            "reader.conf.d/asicen-userland.conf",
        }
    else:
        expected |= {"ndk/NOTICE", "ndk/NOTICE.toolchain", "ndk/source.properties"}
        expected.add(TERMUX_LAUNCHER)
        for program in PROGRAMS:
            expected.add(f"evidence/inventory/{program}-static-archives.tsv")
    return expected


def audit_binary_archive(args: argparse.Namespace) -> dict:
    validate_platform(args.platform)
    if args.platform == WINDOWS_PLATFORM:
        return audit_windows_archive(args)
    members = archive_members(args.archive.resolve())
    expected = expected_binary_members(args.platform)
    if set(members) != expected:
        fail(f"archive member allowlist mismatch; unexpected={sorted(set(members)-expected)}, missing={sorted(expected-set(members))}")
    for name, member in members.items():
        expected_mode = 0o755 if name in BINARY_EXECUTABLE_MEMBERS else 0o644
        if member.mode != expected_mode:
            fail(f"binary archive mode mismatch for {name}: expected {expected_mode:o}, got {member.mode:o}")
    verify_firmware(read_archive_file(args.archive.resolve(), FIRMWARE_MEMBER),
                    read_archive_file(args.archive.resolve(), FIRMWARE_NOTICE_MEMBER),
                    {name: member.mode for name, member in members.items()})
    if args.platform.startswith("linux-"):
        if members[LINUX_BUILD_METADATA_MEMBER].mode != 0o644:
            fail("Linux static build metadata must have mode 644")
        linux_metadata = verify_linux_build_metadata(
            read_archive_file(args.archive.resolve(), LINUX_BUILD_METADATA_MEMBER),
            {name: hashlib.sha256(read_archive_file(args.archive.resolve(), name)).hexdigest()
             for name in PROGRAMS}, args.platform)
        if members[LINUX_IFD_METADATA_MEMBER].mode != 0o644:
            fail("Linux IFD build metadata must have mode 644")
        ifd_metadata = verify_linux_ifd_metadata(
            read_archive_file(args.archive.resolve(), LINUX_IFD_METADATA_MEMBER),
            hashlib.sha256(read_archive_file(args.archive.resolve(), "ifd/asicen-userland-ifd.so")).hexdigest(),
            args.platform, linux_metadata.get("source_ref"))
        for name, expected_sha256 in LINUX_RUNTIME_LICENSE_SHA256.items():
            if members[name].mode != 0o644 or hashlib.sha256(
                    read_archive_file(args.archive.resolve(), name)).hexdigest() != expected_sha256:
                fail(f"Linux runtime license does not match the pinned text and mode: {name}")
    if members["libusb/COPYING"].mode != 0o644:
        fail(f"binary archive libusb/COPYING must have mode 644, got {members['libusb/COPYING'].mode:o}")
    if hashlib.sha256(read_archive_file(args.archive.resolve(), "libusb/COPYING")).hexdigest() != LIBUSB_COPYING_SHA256:
        fail("binary archive libusb/COPYING does not match the verified libusb 1.0.30 license")
    verify_linux_mdev_modes({name: member.mode for name, member in members.items()}, args.platform)
    if args.platform == "darwin-arm64":
        verify_darwin_bundle_plist(read_archive_file(args.archive.resolve(),
                                  "ifd/asicen-userland-ifd.bundle/Contents/Info.plist"))
    if args.platform.startswith("android"):
        audit_termux_launcher(read_archive_file(args.archive.resolve(), TERMUX_LAUNCHER),
                              members[TERMUX_LAUNCHER].mode)
    audit_archive_elf_sections(args.archive.resolve(), members, args.platform)
    manifest = verify_manifest(args.archive.resolve(), members, args.platform)
    if args.platform.startswith("linux-") and "source_ref" in linux_metadata:
        if linux_metadata["source_ref"] != manifest["source_ref"]:
            fail("Linux static build source_ref does not match the package manifest")
    if args.platform.startswith("linux-") and ifd_metadata["source_ref"] != manifest["source_ref"]:
        fail("Linux IFD build source_ref does not match the package manifest")
    expected_name = f"asicen-userland-{manifest['version']}-{args.platform}.tar.gz"
    if args.archive.name != expected_name:
        fail(f"archive name does not match platform/version: {args.archive.name}")
    if args.platform.startswith("linux-") or args.platform == "darwin-arm64":
        validate_reader_template(read_archive_file(args.archive.resolve(), "reader.conf.d/asicen-userland.conf"))
    notice = read_archive_file(args.archive.resolve(), "DEPENDENCY-NOTICE.txt").decode("utf-8")
    evidence = verify_binary_evidence(args.archive.resolve(), members, args.platform)
    if args.platform.startswith("android"):
        properties = read_archive_file(args.archive.resolve(), "ndk/source.properties").decode("utf-8")
        revision = next((line.split("=", 1)[1].strip() for line in properties.splitlines()
                         if line.startswith("Pkg.Revision = ")), "")
        if not revision.startswith("27."):
            fail(f"Android archive does not retain an NDK r27/r27d revision: {revision or 'unknown'}")
        fields = notice_fields(notice)
        required_fields = {
            "dependency.libusb.version": "1.0.30",
            "dependency.libusb.linkage": "static",
            "dependency.libusb.license": "LGPL-2.1-or-later",
            "dependency.ndk.revision": revision,
            "dependency.ndk.materials": "ndk/source.properties,ndk/NOTICE,ndk/NOTICE.toolchain",
            "corresponding-source-archive": "present",
        }
        for key, value in required_fields.items():
            if fields.get(key) != value:
                fail(f"Android dependency notice field mismatch: {key}={value}")
        android_evidence = evidence["android"]
        if android_evidence["ndk_revision"] != revision:
            fail("Android binary evidence NDK revision does not match source.properties")
        if fields.get("dependency.ndk.revision") != android_evidence["ndk_revision"]:
            fail("Android binary evidence NDK revision does not match dependency notice")
    else:
        fields = notice_fields(notice)
        if (fields.get("dependency.libusb.linkage") != "static" or
                fields.get("dependency.libusb.version") != "1.0.30" or
                fields.get("dependency.libusb.license") != "LGPL-2.1-or-later"):
            fail("native dependency notice does not describe static pinned libusb")
        if args.platform.startswith("linux-") and \
                fields.get("dependency.libc") != ("glibc" if args.platform.startswith("linux-glibc") else "musl"):
            fail("Linux dependency notice libc mismatch")
        if fields.get("corresponding-source-archive") != f"asicen-userland-{manifest['version']}-source.tar.gz":
            fail("native dependency notice must point to the separately published source archive")
    verify_checksums(args.archive.resolve(), members)
    return {"archive": str(args.archive.resolve()), "platform": args.platform, "members": sorted(members), "manifest": manifest}


def audit_source_archive(args: argparse.Namespace) -> dict:
    members = archive_members(args.archive.resolve(), forbidden=None)
    for name in members:
        if not name.startswith(("repository/", "third_party/libusb-1.0.30/")):
            continue
        if SOURCE_FORBIDDEN.search(name):
            fail(f"forbidden source archive member: {name}")
        if SOURCE_WINDOWS_TOKEN.search(name) and not WINDOWS_SOURCE_ALLOWED.fullmatch(name):
            fail(f"forbidden source archive member: {name}")
    required = {
        "BUILD-RELINK.md",
        "source-manifest.json",
        "SHA256SUMS",
        "DEPENDENCY-NOTICE.txt",
        "LICENSE",
        "README.md",
        "THIRD_PARTY_NOTICES.md",
        "third_party/libusb-1.0.30.tar.bz2",
        "repository/LICENSE",
        "repository/VERSION",
        "repository/README.md",
        "repository/THIRD_PARTY_NOTICES.md",
        "repository/scripts/build-linux-static.sh",
        "repository/scripts/build-linux-ifd.sh",
        "repository/scripts/build-macos-static.sh",
        "repository/scripts/test-static-relink.sh",
        "repository/packaging/REBUILD.md.in",
    }
    if any(name.startswith(("repository/third_party/px4-userland/userland/src/windows/",
                            "repository/userland/src/windows/")) for name in members):
        # A committed Windows Phase 1 tree must ship the complete adapter,
        # test, build script, and header coverage; a partial tree is rejected.
        required |= WINDOWS_SOURCE_REQUIRED
    if not required.issubset(members):
        fail(f"source archive is missing: {sorted(required-set(members))}")
    for name in members:
        if name not in required and not name.startswith("repository/") and not name.startswith("third_party/libusb-1.0.30/"):
            fail(f"unexpected source archive member: {name}")
    manifest = read_json_archive_file(args.archive.resolve(), "source-manifest.json")
    if manifest.get("schema") != 1 or manifest.get("libusb_version") != "1.0.30":
        fail("invalid source manifest")
    if manifest.get("kind") != "corresponding-source":
        fail("invalid source manifest kind")
    for field in ("repository_commit", "repository_tree"):
        if not isinstance(manifest.get(field), str) or not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", manifest[field]):
            fail(f"invalid source manifest {field}")
    version = str(manifest.get("version", ""))
    validate_version(version)
    if read_archive_file(args.archive.resolve(), "repository/VERSION") != (version + "\n").encode("ascii"):
        fail("source manifest version differs from the repository VERSION")
    if args.archive.name != f"asicen-userland-{version}-source.tar.gz":
        fail(f"source archive name does not match manifest version: {args.archive.name}")
    inventory = manifest.get("files")
    if not isinstance(inventory, dict):
        fail("source manifest files inventory is missing")
    expected_names = set(members) - {"source-manifest.json", "SHA256SUMS"}
    if set(inventory) != expected_names:
        fail("source manifest files inventory does not match archived payload")
    with tarfile.open(args.archive.resolve(), "r:*") as stream:
        for name in sorted(expected_names):
            record = inventory[name]
            if (not isinstance(record, dict) or set(record) != {"sha256", "size"} or
                    not isinstance(record["sha256"], str) or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]) or
                    not isinstance(record["size"], int) or isinstance(record["size"], bool) or record["size"] < 0):
                fail(f"invalid source manifest file record: {name}")
            payload = stream.extractfile(stream.getmember(name))
            if payload is None:
                fail(f"cannot read source manifest file: {name}")
            data = payload.read()
            if record["size"] != len(data) or record["sha256"] != hashlib.sha256(data).hexdigest():
                fail(f"source manifest file record mismatch: {name}")
            if len(data) == FIRMWARE_SIZE and record["sha256"] == FIRMWARE_SHA256:
                fail(f"vendor firmware bytes are forbidden in corresponding source: {name}")
    if manifest.get("libusb_archive_sha256") != LIBUSB_SHA256:
        fail("source manifest does not record the pinned libusb checksum")
    if "COPYING" not in "\n".join(members):
        fail("libusb COPYING is missing from source archive")
    embedded = read_archive_file(args.archive.resolve(), "third_party/libusb-1.0.30.tar.bz2")
    if hashlib.sha256(embedded).hexdigest() != LIBUSB_SHA256:
        fail("embedded libusb source archive checksum mismatch")
    try:
        with tarfile.open(args.archive.resolve(), "r:*") as source_stream:
            expanded_payloads = {}
            for name in sorted(members):
                if name.startswith("third_party/libusb-1.0.30/"):
                    handle = source_stream.extractfile(name)
                    if handle is None:
                        fail(f"cannot read expanded libusb source: {name}")
                    expanded_payloads[name] = handle.read()
        with tarfile.open(fileobj=io.BytesIO(embedded), mode="r:bz2") as stream:
            names = []
            expanded_names = set()
            for member in stream.getmembers():
                safe_member(member.name)
                if member.name != "libusb-1.0.30" and not member.name.startswith("libusb-1.0.30/"):
                    fail(f"unexpected embedded libusb member: {member.name}")
                if member.isdir():
                    continue
                if member.issym() or member.islnk() or not member.isfile():
                    fail(f"embedded libusb member is not regular: {member.name}")
                if member.name in names:
                    fail(f"duplicate embedded libusb member: {member.name}")
                names.append(member.name)
                expanded_name = f"third_party/{member.name}"
                expanded_names.add(expanded_name)
                if expanded_name not in members:
                    fail(f"expanded libusb source is missing: {expanded_name}")
                handle = stream.extractfile(member)
                if handle is None or expanded_payloads.get(expanded_name) != handle.read():
                    fail(f"expanded libusb source differs from pinned archive: {expanded_name}")
            if "libusb-1.0.30/COPYING" not in names:
                fail("embedded libusb source has no COPYING")
            if expanded_names != set(expanded_payloads):
                fail("expanded libusb source inventory differs from pinned archive")
    except (OSError, tarfile.TarError) as error:
        fail(f"cannot inspect embedded libusb source: {error}")
    source_notice = read_archive_file(args.archive.resolve(), "DEPENDENCY-NOTICE.txt").decode("utf-8")
    source_fields = notice_fields(source_notice)
    if source_fields.get("dependency.libusb.version") != "1.0.30" or \
            source_fields.get("corresponding-source-archive") != "present":
        fail("source dependency notice is missing stable dependency fields")
    verify_checksums(args.archive.resolve(), members)
    return {"archive": str(args.archive.resolve()), "kind": "source", "members": sorted(members), "manifest": manifest}


def self_test() -> int:
    with tempfile.TemporaryDirectory(prefix="asicen-audit-test-") as temporary:
        root = Path(temporary)
        safe = root / "safe.tar.gz"
        with tarfile.open(safe, "w:gz") as stream:
            info = tarfile.TarInfo("README.md")
            payload = b"ok\n"
            info.size = len(payload)
            stream.addfile(info, io.BytesIO(payload))
        members = archive_members(safe)
        if "README.md" not in members:
            fail("self-test could not read safe archive")
        bad = root / "bad.tar.gz"
        with tarfile.open(bad, "w:gz") as stream:
            info = tarfile.TarInfo("../escape")
            info.size = 1
            stream.addfile(info, io.BytesIO(b"x"))
        try:
            archive_members(bad)
        except AuditError:
            pass
        else:
            fail("self-test accepted traversal")

        platform = "linux-musl-x86_64"
        archive_name = f"asicen-userland-0.1.0-{platform}.tar.gz"
        binary_payloads = {
            program: f"synthetic {program}\n".encode("ascii") for program in PROGRAMS
        }
        binary_payloads["ifd/asicen-userland-ifd.so"] = b"synthetic ifd\n"
        build_metadata = {
            "schema": 1, "compiler_target": "x86_64-linux-musl",
            "libc_archive_sha256": "a" * 64, "libc_identification": "musl libc x86_64 Version 1.2.5",
            "source_ref": "self-test",
            "production_binary_sha256": {name: hashlib.sha256(binary_payloads[name]).hexdigest() for name in PROGRAMS},
        }
        ifd_metadata = {
            "schema": 1, "compiler_target": "x86_64-linux-musl", "source_ref": "self-test",
            "library_sha256": hashlib.sha256(binary_payloads["ifd/asicen-userland-ifd.so"]).hexdigest(),
        }
        base_files = {
            "LICENSE": b"license\n",
            "README.md": b"readme\n",
            "THIRD_PARTY_NOTICES.md": b"notices\n",
            "libusb/COPYING": (Path(__file__).resolve().parents[1] / "packaging/libusb/COPYING").read_bytes(),
            "DEPENDENCY-NOTICE.txt": (
                b"dependency.libusb.version=1.0.30\n"
                b"dependency.libusb.linkage=static\n"
                b"dependency.libusb.license=LGPL-2.1-or-later\n"
                b"dependency.libc=musl\n"
                b"dependency.executables=static-musl\n"
                b"corresponding-source-archive=asicen-userland-0.1.0-source.tar.gz\n"
            ),
            "reader.conf.d/asicen-userland.conf": (
                b"DEVICENAME asicen-userland:runtime=@ASICEN_RUNTIME_DIR@:"
                b"instance=@ASICEN_INSTANCE@:access=@ASICEN_ACCESS@\n"
                b"LIBPATH @ASICEN_IFD_LIBRARY@\n"
            ),
            "mdev/asicen-userland-mdev.conf": b"mdev rule\n",
            "mdev/asicen-userland-mdev.sh": b"#!/bin/sh\n",
            "mdev/asicen-userland-mdev.start": b"#!/bin/sh\n",
            FIRMWARE_MEMBER: (Path(__file__).resolve().parents[1] / FIRMWARE_MEMBER).read_bytes(),
            FIRMWARE_NOTICE_MEMBER: FIRMWARE_NOTICE.encode("utf-8"),
            LINUX_BUILD_METADATA_MEMBER: (json.dumps(build_metadata, sort_keys=True) + "\n").encode(),
            LINUX_IFD_METADATA_MEMBER: (json.dumps(ifd_metadata, sort_keys=True) + "\n").encode(),
            **{name: (Path(__file__).resolve().parents[1] / "distribution" / "licenses" /
                      PurePosixPath(name).name).read_bytes() for name in LINUX_RUNTIME_LICENSE_SHA256},
            **binary_payloads,
        }

        def write_test_archive(path: Path, evidence: dict, mode_overrides: dict[str, int] | None = None,
                               file_overrides: dict[str, bytes | None] | None = None) -> None:
            files = dict(base_files)
            if file_overrides:
                for name, payload in file_overrides.items():
                    if payload is None:
                        files.pop(name, None)
                    else:
                        files[name] = payload
            files["evidence/binary-audit.json"] = (
                json.dumps(evidence, indent=2, sort_keys=True) + "\n"
            ).encode("utf-8")
            manifest = {
                "schema": 1,
                "version": "0.1.0",
                "platform": platform,
                "libc": "musl",
                "architecture": "x86_64",
                "embedded_libusb": {"version": "1.0.30", "linkage": "static"},
                "source_ref": "self-test",
                "programs": list(PROGRAMS),
                "production_only": True,
                "files": {
                    name: {"size": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}
                    for name, payload in sorted(files.items())
                },
            }
            files["manifest.json"] = (
                json.dumps(manifest, indent=2, sort_keys=True) + "\n"
            ).encode("utf-8")
            files["SHA256SUMS"] = (
                "\n".join(
                    f"{hashlib.sha256(files[name]).hexdigest()}  {name}"
                    for name in sorted(files)
                ) + "\n"
            ).encode("ascii")
            with tarfile.open(path, "w:gz") as stream:
                for name, payload in sorted(files.items()):
                    info = tarfile.TarInfo(name)
                    info.size = len(payload)
                    default_mode = 0o755 if name in BINARY_EXECUTABLE_MEMBERS else 0o644
                    info.mode = (mode_overrides or {}).get(name, default_mode)
                    stream.addfile(info, io.BytesIO(payload))

        def audit_test_archive(path: Path, section_mode: str = "stripped") -> None:
            testdata = Path(__file__).resolve().parent / "testdata"
            previous_path = os.environ.get("PATH")
            previous_sections = os.environ.get("ASICEN_TEST_SECTIONS")
            os.environ["PATH"] = f"{testdata}{os.pathsep}{previous_path or ''}"
            os.environ["ASICEN_TEST_SECTIONS"] = section_mode
            try:
                audit_binary_archive(argparse.Namespace(archive=path, platform=platform))
            finally:
                if previous_path is None:
                    os.environ.pop("PATH", None)
                else:
                    os.environ["PATH"] = previous_path
                if previous_sections is None:
                    os.environ.pop("ASICEN_TEST_SECTIONS", None)
                else:
                    os.environ["ASICEN_TEST_SECTIONS"] = previous_sections

        def evidence_for(files: dict[str, bytes]) -> dict:
            def record(name: str) -> dict:
                return {
                    "artifact": name,
                    "format": "ELF",
                    "interpreter": None,
                    "needed": [],
                    "libc": "musl",
                    "glibc_floor": None,
                    "sha256": hashlib.sha256(files[name]).hexdigest(),
                }

            return {
                "platform": platform,
                "programs": [record(program) for program in PROGRAMS],
                "extra": [record("ifd/asicen-userland-ifd.so")],
            }

        valid = root / archive_name
        valid_evidence = evidence_for(base_files)
        write_test_archive(valid, valid_evidence)
        audit_test_archive(valid)
        audit_test_archive(valid, "dynsym-unwind")
        audit_test_archive(valid, "build-id-non-asicend")

        expected_mdev_modes = {
            "mdev/asicen-userland-mdev.conf": 0o644,
            "mdev/asicen-userland-mdev.sh": 0o755,
            "mdev/asicen-userland-mdev.start": 0o755,
        }
        for name, expected_mode in expected_mdev_modes.items():
            invalid_mode = 0o600 if name.endswith(".conf") else 0o744
            invalid_archive = root / f"invalid-{name.rsplit('/', 1)[-1]}.tar.gz"
            write_test_archive(invalid_archive, valid_evidence, {name: invalid_mode})
            try:
                audit_test_archive(invalid_archive)
            except AuditError:
                pass
            else:
                fail(f"invalid mdev mode archive self-test did not fail: {name}")

        invalid_license_mode_archive = root / "invalid-libusb-copying-mode.tar.gz"
        write_test_archive(invalid_license_mode_archive, valid_evidence,
                           {"libusb/COPYING": 0o600})
        try:
            audit_test_archive(invalid_license_mode_archive)
        except AuditError:
            pass
        else:
            fail("invalid libusb/COPYING mode archive self-test did not fail")

        for section_mode in ("debug", "zdebug", "symtab", "build-id-asicend"):
            try:
                audit_test_archive(valid, section_mode)
            except AuditError:
                pass
            else:
                fail(f"self-test accepted {section_mode} sections")

        invalid_readers = {
            "missing-placeholder": base_files["reader.conf.d/asicen-userland.conf"].replace(
                b":access=@ASICEN_ACCESS@", b":access=user"
            ),
            "extra-placeholder": base_files["reader.conf.d/asicen-userland.conf"] + b"# @ASICEN_EXTRA@\n",
        }
        for description, reader in invalid_readers.items():
            try:
                validate_reader_template(reader)
            except AuditError:
                pass
            else:
                fail(f"self-test accepted reader template with {description}")

        def expect_archive_rejected(path: Path, description: str) -> None:
            try:
                audit_test_archive(path)
            except AuditError:
                return
            fail(f"self-test accepted {description}")

        tampered_evidence = dict(valid_evidence)
        tampered_evidence["platform"] = "darwin-arm64"
        tampered_evidence_archive = root / "tampered-evidence" / archive_name
        tampered_evidence_archive.parent.mkdir()
        write_test_archive(tampered_evidence_archive, tampered_evidence)
        expect_archive_rejected(tampered_evidence_archive, "tampered evidence")

        tampered_hash = json.loads(json.dumps(valid_evidence))
        tampered_hash["programs"][0]["sha256"] = "0" * 64
        tampered_hash_archive = root / "tampered-hash" / archive_name
        tampered_hash_archive.parent.mkdir()
        write_test_archive(tampered_hash_archive, tampered_hash)
        expect_archive_rejected(tampered_hash_archive, "tampered evidence hash")

        duplicate_program = json.loads(json.dumps(valid_evidence))
        duplicate_program["programs"][1]["artifact"] = "asicend"
        duplicate_program_archive = root / "duplicate-program" / archive_name
        duplicate_program_archive.parent.mkdir()
        write_test_archive(duplicate_program_archive, duplicate_program)
        expect_archive_rejected(duplicate_program_archive, "duplicate program artifact")

        missing_license_archive = root / "missing-license" / archive_name
        missing_license_archive.parent.mkdir()
        write_test_archive(missing_license_archive, valid_evidence, file_overrides={"libusb/COPYING": None})
        expect_archive_rejected(missing_license_archive, "archive missing libusb/COPYING")

        tampered_license_archive = root / "tampered-license" / archive_name
        tampered_license_archive.parent.mkdir()
        write_test_archive(tampered_license_archive, valid_evidence,
                           file_overrides={"libusb/COPYING": base_files["libusb/COPYING"] + b"tampered\n"})
        expect_archive_rejected(tampered_license_archive, "tampered libusb/COPYING")
        for name in (FIRMWARE_MEMBER, FIRMWARE_NOTICE_MEMBER, *LINUX_RUNTIME_LICENSE_SHA256):
            for description, replacement in (("missing", None), ("tampered", b"tampered\n")):
                invalid_archive = root / f"{description}-{PurePosixPath(name).name}" / archive_name
                invalid_archive.parent.mkdir()
                write_test_archive(invalid_archive, valid_evidence, file_overrides={name: replacement})
                expect_archive_rejected(invalid_archive, f"{description} {name}")
            invalid_archive = root / f"mode-{PurePosixPath(name).name}" / archive_name
            invalid_archive.parent.mkdir()
            write_test_archive(invalid_archive, valid_evidence, {name: 0o755})
            expect_archive_rejected(invalid_archive, f"invalid mode {name}")
    print("artifact audit self-test: traversal, evidence, missing license, and license tampering rejected")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--ifd-smoke-library", type=Path)
    parser.add_argument("--ifd-smoke-daemon", type=Path)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--platform", choices=sorted(PLATFORMS))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--binary-suffix", default="")
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--source-archive", action="store_true")
    parser.add_argument("--ifd-library", type=Path)
    parser.add_argument("--ifd-bundle", type=Path)
    parser.add_argument("--link-map-dir", type=Path)
    parser.add_argument("--ndk-root", type=Path)
    parser.add_argument("--inventory-dir", type=Path)
    parser.add_argument("--evidence-output", type=Path)
    args = parser.parse_args()
    if args.ifd_smoke_library or args.ifd_smoke_daemon:
        if not args.ifd_smoke_library or not args.ifd_smoke_daemon:
            parser.error("--ifd-smoke-library and --ifd-smoke-daemon are required together")
        return audit_ifd_proxy(args.ifd_smoke_library.resolve(), args.ifd_smoke_daemon.resolve())
    if args.self_test:
        return self_test()
    if args.source_archive and not args.archive:
        parser.error("--source-archive requires --archive")
    if args.source_archive:
        result = audit_source_archive(args)
    elif args.archive:
        result = audit_binary_archive(args)
        if args.build_dir:
            result = {"archive": result, "binaries": audit_binaries(args)}
    elif args.build_dir and args.platform:
        result = {"binaries": audit_binaries(args)}
    else:
        parser.error("provide --archive, or --platform and --build-dir")
    if args.evidence_output:
        args.evidence_output.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"audited {args.archive}" if args.archive else "audited binaries")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AuditError, OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"artifact audit: {error}", file=sys.stderr)
        raise SystemExit(1)
