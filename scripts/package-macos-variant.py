#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Add the externally supplied firmware to an audited macOS intermediate candidate."""
import argparse
import gzip
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
AUDIT_PATH = ROOT / "scripts/audit-macos-candidate.py"
SPEC = importlib.util.spec_from_file_location("asicen_macos_audit", AUDIT_PATH)
AUDIT = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(AUDIT)


def fail(message: str) -> None:
    raise ValueError(message)


def write_sums(stage: Path) -> None:
    lines = []
    for path in sorted(item for item in stage.rglob("*") if item.is_file() and item.name != "SHA256SUMS"):
        relative = path.relative_to(stage).as_posix()
        lines.append(f"{AUDIT.digest(path)}  {relative}")
    (stage / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_tar(stage: Path, destination: Path) -> None:
    raw_path = destination.with_suffix(".tar")
    with tarfile.open(raw_path, "w", format=tarfile.PAX_FORMAT) as archive:
        for path in [stage, *sorted(stage.rglob("*"))]:
            relative = "." if path == stage else path.relative_to(stage).as_posix()
            info = archive.gettarinfo(str(path), arcname=relative)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mode = 0o755 if path.is_dir() or path.stat().st_mode & 0o111 else 0o644
            if path.is_file():
                with path.open("rb") as stream:
                    archive.addfile(info, stream)
            else:
                archive.addfile(info)
    with raw_path.open("rb") as source, destination.open("wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output,
                           compresslevel=9, mtime=0) as compressor:
            shutil.copyfileobj(source, compressor)
    raw_path.unlink()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.candidate.is_symlink():
            fail("candidate directory must not be a symlink")
        candidate = args.candidate.resolve(strict=True)
        firmware = args.firmware
        output = args.output
        AUDIT.audit_candidate(candidate, final=False)
        if firmware.is_symlink() or not firmware.is_file():
            fail("firmware must be a regular, non-symlink file")
        firmware = firmware.resolve(strict=True)
        if firmware.is_relative_to(ROOT.resolve()):
            fail("firmware must be supplied from outside the source checkout")
        if firmware.stat().st_size != 16384 or AUDIT.digest(firmware) != AUDIT.FIRMWARE_SHA256:
            fail("firmware does not match the recorded external component")
        parent = output.parent.resolve(strict=True)
        normalized_output = parent / output.name
        if normalized_output.is_relative_to(ROOT.resolve()) or normalized_output.exists() or normalized_output.is_symlink():
            fail("output must be a new external path")
        archive_path = Path(str(normalized_output) + ".tar.gz")
        if archive_path.exists() or archive_path.is_symlink():
            fail("output archive already exists")
        with tempfile.TemporaryDirectory(prefix="asicen-macos-final-", dir=parent) as temporary:
            stage = Path(temporary) / "stage"
            shutil.copytree(candidate, stage, symlinks=False)
            (stage / "firmware").mkdir()
            shutil.copyfile(firmware, stage / "firmware/asicen-loader.bin")
            (stage / "firmware/asicen-loader.bin").chmod(0o644)
            notice = (
                "Vendor component: PLEX loader firmware, distributed separately from this project's license.\n"
                "Source artifact SHA-256: 11a84eaef0157ac08c0b4128aa622a625e8914a28c59ef06e76378ee6094c5de\n"
                "Original loader object: loader.ko, SHA-256 10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0\n"
                f"Extracted component: FirmBin, 16384 bytes, SHA-256 {AUDIT.FIRMWARE_SHA256}\n"
                "Redistribution rights: unresolved. Inclusion reflects the user's explicit candidate-distribution choice and does not assert that rights are cleared.\n"
                "This file is not relicensed under GPL, MIT, public domain, or this project's terms.\n"
            )
            (stage / "licenses/VENDOR-FIRMWARE-NOTICE.txt").write_text(notice, encoding="utf-8")
            metadata = stage / "BUILD-METADATA.txt"
            text = metadata.read_text(encoding="utf-8")
            text = text.replace("Candidate class: intermediate macOS arm64 candidate",
                                "Candidate class: final macOS arm64 package")
            text = text.replace("Firmware: intentionally absent from this CI candidate",
                                "Firmware: included from external input; vendor redistribution rights unresolved")
            text += f"Firmware SHA-256: {AUDIT.FIRMWARE_SHA256}\n"
            metadata.write_text(text, encoding="utf-8")
            write_sums(stage)
            AUDIT.audit_candidate(stage, final=True)
            write_tar(stage, archive_path)
            AUDIT.audit_candidate_archive(archive_path, final=True)
            shutil.move(str(stage), str(normalized_output))
        print(f"assembled final candidate: {normalized_output}")
        print(f"assembled deterministic archive: {archive_path}")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(f"macOS final package: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
