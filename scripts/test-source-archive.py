#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Isolated corresponding-source archive and offline dependency fixture."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

COMMON_CORE_COMMIT = "b385ce8768259f8c2d69ce148d8a5d921d581847"
FOUNDATION_CORE_COMMIT = "4873d346476e9bac9073bf39ffba807fdc892733"


def run(command: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(command, cwd=cwd, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f"command failed ({' '.join(command)}):\n{result.stdout}")
    return result.stdout


def write(path: Path, data: str | bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data.encode() if isinstance(data, str) else data)


def git_commit(root: Path, message: str) -> str:
    run(["git", "init", "-q"], cwd=root)
    run(["git", "-c", "user.email=fixture@example.invalid", "-c", "user.name=fixture",
         "add", "-A"], cwd=root)
    run(["git", "-c", "user.email=fixture@example.invalid", "-c", "user.name=fixture",
         "commit", "-q", "-m", message], cwd=root)
    return run(["git", "-C", str(root), "rev-parse", "HEAD"]).strip()


def extract(archive: Path, destination: Path) -> None:
    with tarfile.open(archive, "r:gz") as stream:
        stream.extractall(destination, filter="data")


def rebuild_archive(root: Path, archive: Path) -> None:
    manifest_path = root / "source-manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    inventory = {}
    for path in sorted(root.rglob("*")):
        if path.is_file() and path.name not in ("source-manifest.json", "SHA256SUMS"):
            inventory[path.relative_to(root).as_posix()] = {
                "size": path.stat().st_size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
    manifest["files"] = inventory
    manifest_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n",
                             encoding="utf-8")
    rows = [f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(root).as_posix()}"
            for path in sorted(root.rglob("*"))
            if path.is_file() and path.name != "SHA256SUMS"]
    (root / "SHA256SUMS").write_text("\n".join(rows) + "\n", encoding="utf-8")
    archive.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "w:gz") as stream:
        for path in sorted(root.rglob("*")):
            if path.is_file():
                stream.add(path, arcname=path.relative_to(root).as_posix())


def load_audit(script: Path):
    spec = importlib.util.spec_from_file_location("fixture_audit", script)
    if spec is None or spec.loader is None:
        raise SystemExit("cannot load audit-artifact.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expect_rejected(audit, archive: Path, label: str) -> None:
    try:
        audit.audit_source_archive(argparse.Namespace(archive=archive))
    except audit.AuditError:
        return
    raise SystemExit(f"auditor accepted {label}")


def make_product(base: Path, name: str, *, module: Path | None,
                 lock: str | None, notices: Path,
                 export_ignore: bool = True, receiver: bool = True,
                 card: bool = True) -> Path:
    root = base / name
    write(root / "VERSION", "0.0.0\n")
    write(root / "LICENSE", "fixture product license\n")
    write(root / "README.md", "fixture product readme\n")
    shutil.copyfile(notices, root / "THIRD_PARTY_NOTICES.md")
    for filename in ("build-linux-static.sh", "build-linux-ifd.sh", "build-macos-static.sh",
                     "test-static-relink.sh"):
        write(root / "scripts" / filename, "#!/bin/sh\nexit 0\n")
    write(root / "packaging/REBUILD.md.in", "@VERSION@ @COMMIT@ @TREE@\n")
    write(root / "packaging/DEPENDENCY-NOTICE.txt.in", "@VERSION@ @PLATFORM@\n@DEPENDENCY_TEXT@")
    if module is None:
        write(root / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.16)\nproject(old LANGUAGES CXX)\n")
    else:
        (root / "cmake").mkdir(parents=True)
        shutil.copyfile(module, root / "cmake/DtvUserlandCoreDependency.cmake")
        if lock is not None:
            write(root / "cmake/dtv-userland-core.revision", lock + "\n")
        text = module.read_text(encoding="utf-8")
        functions = re.findall(r"function\(([A-Za-z0-9_]*add_dtv_userland_core)", text)
        if not functions:
            raise SystemExit("dependency module has no add_dtv_userland_core function")
        write(root / "CMakeLists.txt", (
            "cmake_minimum_required(VERSION 3.16)\nproject(closure LANGUAGES CXX)\n"
            "include(cmake/DtvUserlandCoreDependency.cmake)\n"
            f"{functions[-1]}()\n"
            "add_executable(closure main.cpp)\n"
            + ("target_link_libraries(closure PRIVATE dtv::core" +
               (" dtv::receiver" if receiver else "") +
               (" dtv::card" if card else "") + ")\n")
        ))
        write(root / "main.cpp", (
            '#include "dtv/userland/detail/receiver_engine.h"\n'
            '#include "dtv/userland/detail/card_ownership_engine.h"\n'
            '#include <array>\n'
            "using namespace dtv::userland;\n"
            "using namespace dtv::userland::detail;\n"
            "class B final : public ReceiverBackend { public:\n"
            "bool supports(ReceiverId, ReceiverSystem) const noexcept override { return true; }\n"
            "bool selects_satellite_stream_before_tune() const noexcept override { return false; }\n"
            "std::uint32_t lock_settle_ms(ReceiverId, ReceiverSystem) const noexcept override { return 0U; }\n"
            "Result<void> open(ReceiverId) noexcept override { return Result<void>::success(); }\n"
            "Result<void> tune(ReceiverId, ReceiverSystem, std::uint32_t, std::uint32_t) noexcept override { return Result<void>::success(); }\n"
            "Result<bool> is_locked(ReceiverId, ReceiverSystem) noexcept override { return Result<bool>::success(true); }\n"
            "Result<void> select_satellite_slot(ReceiverId, std::uint8_t, std::uint32_t) noexcept override { return Result<void>::success(); }\n"
            "Result<void> select_satellite_tsid(ReceiverId, std::uint16_t, std::uint32_t) noexcept override { return Result<void>::success(); }\n"
            "Result<void> close(ReceiverId) noexcept override { return Result<void>::success(); }\n"
            "Result<void> begin_tune_power(ReceiverId, ReceiverSystem, std::uint8_t) noexcept override { return Result<void>::success(); }\n"
            "Result<void> commit_tune_power(ReceiverId) noexcept override { return Result<void>::success(); }\n"
            "Result<void> rollback_tune_power(ReceiverId) noexcept override { return Result<void>::success(); }\n"
            "void mark_disconnected(ReceiverId) noexcept override {}\n"
            "Result<void> shutdown() noexcept override { return Result<void>::success(); }\n"
            "Result<void> start_capture(ReceiverId, ReceiverSystem) noexcept override { return Result<void>::success(); }\n"
            "Result<void> stop_capture(ReceiverId, ReceiverSystem) noexcept override { return Result<void>::success(); } };\n"
            "class T final : public ReceiverTokenSource { public: Result<ReceiverToken> generate() noexcept override { return Result<ReceiverToken>::success(ReceiverToken{1U}); } };\n"
            "class C final : public ReceiverClock { public: std::uint64_t monotonic_ms() noexcept override { return 0U; } void sleep_ms(std::uint32_t) noexcept override {} };\n"
            "class CB final : public CardOwnershipBackend { public: Result<void> set_power(bool) noexcept override { return Result<void>::success(); } Result<void> initialize_uart() noexcept override { return Result<void>::success(); } Result<bool> detect_card() noexcept override { return Result<bool>::success(true); } };\n"
            "class CS final : public CardOwnershipSession<std::uint32_t> { public: Result<void> initialize() noexcept override { initialized_ = true; return Result<void>::success(); } Result<std::size_t> transmit(ByteSpan, MutableByteSpan) noexcept override { return Result<std::size_t>::success(0U); } bool initialized() const noexcept override { return initialized_; } const std::uint32_t& atr() const noexcept override { return atr_; } void invalidate() noexcept override { initialized_ = false; } private: std::uint32_t atr_ = 0U; bool initialized_ = false; };\n"
            "int main() { B backend; T tokens; C clock; const std::array<ReceiverId, 1> ids{513U}; ReceiverEngine engine(backend, ids.data(), ids.size(), tokens, clock); const auto lease = engine.acquire(1U, 513U); TuneRequest request{}; request.lease_id = lease ? lease.value().lease_id : 0U; request.frequency_khz = 557000U; request.timeout_ms = 1000U; CB card_backend; CS card_session; CardOwnershipEngine<std::uint32_t> card(card_backend, card_session, 255U); return lease && engine.tune(1U, request) && engine.release(1U, request.lease_id) && card.shutdown() ? 0 : 1; }\n"
            if receiver else
            '#include "dtv/userland/error.h"\n'
            "int main() { return dtv::userland::Result<void>::success() ? 0 : 1; }\n"
        ))
        write(root / "PROVENANCE.md", (
            "dtv-userland-core commit " + (lock or COMMON_CORE_COMMIT) + "; MIT license.\n"
        ))
    if export_ignore:
        write(root / ".gitattributes", "firmware/ export-ignore\nfirmware/** export-ignore\n")
    write(root / "firmware/asicen-loader.bin", b"fixture-only vendor firmware sentinel")
    git_commit(root, name)
    return root


def package(helper: Path, product: Path, libusb: Path, output: Path,
            common: Path | None) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, str(helper), "--version", "0.0.0", "--source-root", str(product),
               "--source-ref", "HEAD", "--libusb-source-archive", str(libusb),
               "--output-dir", str(output)]
    if common is not None:
        command += ["--common-core-source-root", str(common)]
    return subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--libusb-source-archive", type=Path, required=True)
    parser.add_argument("--common-core-source-root", type=Path, required=True)
    args = parser.parse_args()

    scripts = Path(__file__).resolve().parent
    product_root = scripts.parent
    helper = scripts / "package-source.py"
    module = product_root / "cmake/DtvUserlandCoreDependency.cmake"
    common = args.common_core_source_root.resolve()
    libusb = args.libusb_source_archive.resolve()
    pin = run(["git", "-C", str(common), "rev-parse", "HEAD"]).strip()
    if pin != COMMON_CORE_COMMIT:
        raise SystemExit(f"unexpected dtv-userland-core checkout: {pin}")
    run(["git", "-C", str(common), "cat-file", "-e", f"{FOUNDATION_CORE_COMMIT}^{{commit}}"])
    audit = load_audit(scripts / "audit-artifact.py")

    with tempfile.TemporaryDirectory(prefix="asicen-source-fixture-") as temporary:
        base = Path(temporary)
        product = make_product(base, "product", module=module, lock=pin,
                               notices=product_root / "THIRD_PARTY_NOTICES.md")
        # Packaging must read the committed snapshot lock, not this dirty value.
        write(product / "cmake/dtv-userland-core.revision", "f" * 40 + "\n")
        output = base / "out"
        completed = package(helper, product, libusb, output, common)
        if completed.returncode:
            raise SystemExit(f"source packaging failed:\n{completed.stdout}")
        archive = output / "asicen-userland-0.0.0-source.tar.gz"
        extracted = base / "extracted"
        extracted.mkdir()
        extract(archive, extracted)
        bundled = extracted / "repository/third_party/dtv-userland-core"
        if not (bundled / "CMakeLists.txt").is_file() or not (bundled / "LICENSE").is_file() or \
                not (bundled / "PROVENANCE.json").is_file():
            raise SystemExit("corresponding source omitted the common foundation build/license/provenance")
        if not (bundled / "include/dtv/userland/detail/card_ownership_engine.h").is_file() or \
                not (bundled / "include/dtv/userland/detail/card_ownership_engine.tpp").is_file():
            raise SystemExit("corresponding source omitted the card ownership engine template closure")
        staged_lock = (extracted / "repository/cmake/dtv-userland-core.revision").read_text().strip()
        if staged_lock != pin:
            raise SystemExit("archive used the dirty working-tree lock instead of the selected snapshot lock")
        with tarfile.open(archive, "r:gz") as stream:
            if any(name.startswith("repository/firmware/") for name in stream.getnames()):
                raise SystemExit("corresponding source included excluded vendor firmware")
            if "repository/firmware/asicen-loader.bin" in stream.getnames():
                raise SystemExit("corresponding source included the vendor firmware sentinel")

        # The extracted product must configure and link using its bundled tree,
        # without network access or a source override.
        run(["cmake", "-S", str(extracted / "repository"), "-B", str(base / "offline-build"),
             "-DFETCHCONTENT_FULLY_DISCONNECTED=ON", "-DDTV_USERLAND_BUILD_TESTING=OFF"])
        run(["cmake", "--build", str(base / "offline-build"), "--parallel", "2"])
        run([str(base / "offline-build" / "closure")])

        # Regenerate hashes after removing source, so rejection tests exercise
        # the auditor's required build closure rather than stale checksums.
        missing = base / "missing"
        shutil.copytree(extracted, missing)
        (missing / "repository/third_party/dtv-userland-core/src/receiver_engine.cpp").unlink()
        bad_archive = base / "missing-out/asicen-userland-0.0.0-source.tar.gz"
        rebuild_archive(missing, bad_archive)
        expect_rejected(audit, bad_archive, "a consistent-checksum archive missing the receiver engine source")

        missing_card = base / "missing-card"
        shutil.copytree(extracted, missing_card)
        (missing_card / "repository/third_party/dtv-userland-core/include/dtv/userland/detail/card_ownership_engine.tpp").unlink()
        bad_card_archive = base / "missing-card-out/asicen-userland-0.0.0-source.tar.gz"
        rebuild_archive(missing_card, bad_card_archive)
        expect_rejected(audit, bad_card_archive, "a consistent-checksum archive missing the card ownership engine template")

        unfiltered = make_product(base, "unfiltered", module=module, lock=pin,
                                  notices=product_root / "THIRD_PARTY_NOTICES.md",
                                  export_ignore=False)
        unfiltered_output = base / "unfiltered-out"
        completed = package(helper, unfiltered, libusb, unfiltered_output, common)
        unfiltered_archive = unfiltered_output / "asicen-userland-0.0.0-source.tar.gz"
        if completed.returncode == 0 or not unfiltered_archive.is_file():
            raise SystemExit("production auditor did not reject a source snapshot without firmware export-ignore")
        with tarfile.open(unfiltered_archive, "r:gz") as stream:
            if "repository/firmware/asicen-loader.bin" not in stream.getnames():
                raise SystemExit("unfiltered fixture did not exercise the firmware sentinel path")
        expect_rejected(audit, unfiltered_archive, "a source archive containing unfiltered vendor firmware")

        historical = make_product(base, "historical", module=None, lock=None,
                                  notices=product_root / "THIRD_PARTY_NOTICES.md")
        historical_output = base / "historical-out"
        completed = package(helper, historical, libusb, historical_output, None)
        if completed.returncode:
            raise SystemExit(f"historical source package failed:\n{completed.stdout}")
        historical_archive = historical_output / "asicen-userland-0.0.0-source.tar.gz"
        with tarfile.open(historical_archive, "r:gz") as stream:
            if any("dtv-userland-core" in name for name in stream.getnames()):
                raise SystemExit("historical snapshot unexpectedly bundled the common foundation")

        foundation_module = base / "foundation-dependency.cmake"
        write(foundation_module, (
            "function(asicen_add_dtv_userland_core)\n"
            "  FetchContent_MakeAvailable(dtv_userland_core)\n"
            "endfunction()\n"
        ))
        foundation = make_product(
            base, "foundation-only", module=foundation_module,
            lock=FOUNDATION_CORE_COMMIT, notices=product_root / "THIRD_PARTY_NOTICES.md",
            receiver=False, card=False)
        foundation_archive_dir = base / "foundation-out"
        completed = package(helper, foundation, libusb, foundation_archive_dir, common)
        if completed.returncode:
            raise SystemExit(f"4873 foundation source package failed:\n{completed.stdout}")
        foundation_archive = foundation_archive_dir / "asicen-userland-0.0.0-source.tar.gz"
        with tarfile.open(foundation_archive, "r:gz") as stream:
            names = stream.getnames()
            if "repository/cmake/dtv-userland-core.revision" not in names:
                raise SystemExit("4873 foundation fixture omitted its dependency lock")
            foundation_lock = stream.extractfile(
                "repository/cmake/dtv-userland-core.revision").read().decode().strip()
            if foundation_lock != FOUNDATION_CORE_COMMIT:
                raise SystemExit("4873 foundation fixture has the wrong dependency lock")
            if "repository/third_party/dtv-userland-core/src/receiver_engine.cpp" in names or \
                    "repository/third_party/dtv-userland-core/include/dtv/userland/detail/receiver_engine.h" in names:
                raise SystemExit("4873 foundation fixture unexpectedly requires receiver sources")
            if any("card_ownership_engine" in name for name in names):
                raise SystemExit("4873 foundation fixture unexpectedly requires card engine sources")

        bad_pin = make_product(base, "bad-pin", module=module, lock="f" * 40,
                               notices=product_root / "THIRD_PARTY_NOTICES.md")
        completed = package(helper, bad_pin, libusb, base / "bad-pin-out", common)
        if completed.returncode == 0:
            raise SystemExit("packager accepted a commit absent from the supplied Git checkout")

    print("ASICEN source archive fixture: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
