#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package a firmware-free, source-matched macOS arm64 candidate."""
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
AUDIT_SPEC = importlib.util.spec_from_file_location("audit_macos_candidate", AUDIT_PATH)
if AUDIT_SPEC is None or AUDIT_SPEC.loader is None:
    raise RuntimeError("cannot load macOS candidate auditor")
AUDIT_MODULE = importlib.util.module_from_spec(AUDIT_SPEC)
AUDIT_SPEC.loader.exec_module(AUDIT_MODULE)
LIBUSB_SHA256 = AUDIT_MODULE.LIBUSB_SHA256
audit_candidate = AUDIT_MODULE.audit_candidate
digest = AUDIT_MODULE.digest


def fail(message: str) -> None:
    raise ValueError(message)


def source_names(path: Path) -> list[str]:
    with tarfile.open(path, "r:gz") as archive:
        return sorted(member.name for member in archive.getmembers())


def copy_regular(source: Path, destination: Path, executable: bool = False) -> None:
    if source.is_symlink() or not source.is_file():
        fail(f"required build input is not a regular file: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, destination)
    destination.chmod(0o755 if executable else 0o644)


def normalize_candidate_modes(stage: Path) -> None:
    executable_paths = {
        "bin/asicend", "bin/asicen-ts", "bin/asicenctl",
        "pcsc/ASICEN-IFD.bundle/Contents/MacOS/libifd-asicen.dylib",
    }
    stage.chmod(0o755)
    for path in stage.rglob("*"):
        if path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            relative = path.relative_to(stage).as_posix()
            path.chmod(0o755 if relative in executable_paths else 0o644)


def archive_tree(paths: list[tuple[Path, str]], destination: Path) -> None:
    raw_path = destination.with_suffix(".tar")
    with tarfile.open(raw_path, "w", format=tarfile.PAX_FORMAT) as archive:
        for source, name in paths:
            info = archive.gettarinfo(str(source), arcname=name)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mode = 0o755 if source.is_dir() else 0o644
            if source.is_file():
                with source.open("rb") as stream:
                    archive.addfile(info, stream)
            else:
                archive.addfile(info)
    with raw_path.open("rb") as source, destination.open("wb") as output:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output,
                           compresslevel=9, mtime=0) as compressor:
            shutil.copyfileobj(source, compressor)
    raw_path.unlink()


def write_candidate_archive(stage: Path, destination: Path) -> None:
    entries = [stage, *sorted(stage.rglob("*"))]
    raw_path = destination.with_suffix(".tar")
    with tarfile.open(raw_path, "w", format=tarfile.PAX_FORMAT) as archive:
        for item in entries:
            relative = "." if item == stage else item.relative_to(stage).as_posix()
            info = archive.gettarinfo(str(item), arcname=relative)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mode = 0o755 if item.is_dir() or item.stat().st_mode & 0o111 else 0o644
            if item.is_file():
                with item.open("rb") as stream:
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
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--libusb-source-dir", required=True, type=Path)
    parser.add_argument("--source-snapshot", required=True, type=Path)
    parser.add_argument("--libusb-archive", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        for path in (args.source_snapshot, args.libusb_archive, args.build_dir,
                     args.libusb_source_dir):
            if path.is_symlink():
                fail(f"build input must not be a symlink: {path}")
        source = args.source_snapshot.resolve(strict=True)
        libusb_archive = args.libusb_archive.resolve(strict=True)
        libusb_source = args.libusb_source_dir.resolve(strict=True)
        build_dir = args.build_dir.resolve(strict=True)
        if source.is_relative_to(ROOT.resolve()) or libusb_archive.is_relative_to(ROOT.resolve()):
            fail("source snapshot and libusb archive must be outside the mutable checkout")
        if digest(libusb_archive) != LIBUSB_SHA256:
            fail("libusb archive hash is not the pinned 1.0.30 source")
        output = args.output
        parent = output.parent.resolve(strict=True)
        normalized = parent / output.name
        if normalized.is_relative_to(ROOT.resolve()) or normalized.exists() or normalized.is_symlink():
            fail("candidate output must be a new path outside the source checkout")
        archive_output = Path(str(normalized) + ".tar.gz")
        if archive_output.exists() or archive_output.is_symlink():
            fail("candidate archive already exists")
        bundle = build_dir / "ASICEN-IFD.bundle"
        if not bundle.is_dir():
            fail("the native ASICEN IFD bundle was not built")
        for name in ("asicend", "asicen-ts", "asicenctl"):
            if not (build_dir / name).is_file():
                fail(f"command binary missing from build: {name}")
        source_sha = digest(source)
        with tempfile.TemporaryDirectory(prefix="asicen-macos-package-", dir=parent) as temporary:
            stage = Path(temporary) / "stage"
            (stage / "bin").mkdir(parents=True)
            for name in ("asicend", "asicen-ts", "asicenctl"):
                copy_regular(build_dir / name, stage / "bin" / name, executable=True)
            shutil.copytree(bundle, stage / "pcsc/ASICEN-IFD.bundle")
            (stage / "source").mkdir()
            copy_regular(source, stage / "source/asicen-userland-source.tar.gz")
            copy_regular(libusb_archive, stage / "source/libusb-1.0.30.tar.bz2")
            (stage / "licenses").mkdir()
            with tarfile.open(libusb_archive, "r:bz2") as archive:
                member = next((item for item in archive.getmembers()
                               if item.name.endswith("/COPYING") or item.name == "COPYING"), None)
                if member is None or not member.isfile():
                    fail("pinned libusb source archive lacks COPYING")
                stream = archive.extractfile(member)
                if stream is None:
                    fail("could not read libusb COPYING")
                (stage / "licenses/libusb-COPYING").write_bytes(stream.read())
            copy_regular(ROOT / "COPYING.gpl2", stage / "licenses/COPYING.gpl2")
            copy_regular(ROOT / "third_party/px4-userland/LICENSE",
                         stage / "licenses/px4-userland-GPL-2.0-only.txt")
            copy_regular(ROOT / "NOTICES.md", stage / "licenses/NOTICES.md")
            (stage / "SOURCE-MANIFEST.txt").write_text(
                "\n".join(source_names(source)) + "\n", encoding="utf-8")
            rebuild = (
                "# Rebuild this macOS arm64 candidate\n\n"
                f"Project source archive SHA-256: {source_sha}\n"
                f"Pinned libusb 1.0.30 archive SHA-256: {LIBUSB_SHA256}\n\n"
                "Requires macOS 14 or newer on Apple silicon, Xcode Command Line Tools, "
                "CMake, Ninja, GNU make, Python 3.12, and the macOS PC/SC SDK headers.\n\n"
                "    mkdir project-src libusb-src libusb-prefix\n"
                "    tar -xzf source/asicen-userland-source.tar.gz -C project-src\n"
                "    tar -xjf source/libusb-1.0.30.tar.bz2 -C libusb-src --strip-components=1\n"
                "    cd libusb-src\n"
                "    ./configure --prefix=\"$PWD/../libusb-prefix\" --disable-shared --enable-static --disable-examples-build --disable-tests-build --disable-dependency-tracking\n"
                "    make && make install\n"
                "    cd ../project-src\n"
                "    MACOSX_DEPLOYMENT_TARGET=14.0 cmake -S . -B ../build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DASICEN_ENABLE_LIBUSB=ON -DASICEN_ENABLE_IFD=ON -DASICEN_REQUIRE_IFD=ON -DASICEN_BUILD_TESTS=OFF -DASICEN_LIBUSB_INCLUDE_DIR=\"$PWD/../libusb-prefix/include/libusb-1.0\" -DASICEN_LIBUSB_LIBRARY=\"$PWD/../libusb-prefix/lib/libusb-1.0.a\" '-DASICEN_LIBUSB_EXTRA_LINK_OPTIONS=SHELL:-framework IOKit;SHELL:-framework CoreFoundation;SHELL:-framework Security;-lobjc'\n"
                "    cmake --build ../build --target asicend asicen-ts asicenctl asicen-ifd-bundle\n\n"
                "Modify the extracted libusb source, rebuild it, then relink all three commands with the modified include directory and static archive. This manual LGPL relink path does not require the proprietary firmware. The candidate is an intermediate firmware-free artifact; use the separate external-input assembler for a final package.\n"
            )
            (stage / "REBUILD.md").write_text(rebuild, encoding="utf-8")
            meta = (
                "Candidate class: intermediate macOS arm64 candidate\n"
                "Architecture: arm64\n"
                f"macOS runner: {subprocess.run(['sw_vers', '-productVersion'], check=True, text=True, capture_output=True).stdout.strip()}\n"
                f"Compiler: {subprocess.run(['clang++', '--version'], check=True, text=True, capture_output=True).stdout.splitlines()[0]}\n"
                f"Xcode: {subprocess.run(['xcodebuild', '-version'], check=True, text=True, capture_output=True).stdout.splitlines()[0]}\n"
                "macOS deployment target: 14.0\n"
                f"Source archive SHA-256: {source_sha}\n"
                f"libusb 1.0.30 archive SHA-256: {LIBUSB_SHA256}\n"
                "libusb linkage: static; libusb_init/libusb_open/libusb_close symbols verified before stripping\n"
                "Mach-O audit: arm64 only; system dependencies only; LC_RPATH absent\n"
                "Firmware: intentionally absent from this CI candidate\n"
            )
            (stage / "BUILD-METADATA.txt").write_text(meta, encoding="utf-8")
            normalize_candidate_modes(stage)
            source_bundle = stage / "source/asicen-macos-corresponding-source.tar.gz"
            paths = [
                (stage / "source/asicen-userland-source.tar.gz", "source/asicen-userland-source.tar.gz"),
                (stage / "source/libusb-1.0.30.tar.bz2", "source/libusb-1.0.30.tar.bz2"),
                (stage / "SOURCE-MANIFEST.txt", "SOURCE-MANIFEST.txt"),
                (stage / "REBUILD.md", "REBUILD.md"),
            ]
            paths.extend((p, p.relative_to(stage).as_posix())
                         for p in sorted((stage / "licenses").rglob("*")) if p.is_file())
            archive_tree(paths, source_bundle)
            sums = []
            normalize_candidate_modes(stage)
            for path in sorted(p for p in stage.rglob("*") if p.is_file() and p.name != "SHA256SUMS"):
                sums.append(f"{digest(path)}  {path.relative_to(stage).as_posix()}")
            (stage / "SHA256SUMS").write_text("\n".join(sums) + "\n", encoding="utf-8")
            audit_candidate(stage, final=False, run_smoke=True)
            write_candidate_archive(stage, archive_output)
            shutil.move(str(stage), str(normalized))
        print(f"created macOS arm64 intermediate: {normalized}")
        print(f"created candidate archive: {archive_output}")
        return 0
    except (OSError, ValueError, subprocess.SubprocessError, tarfile.TarError) as error:
        print(f"macOS candidate packaging: FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
