#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Rebuild the extracted corresponding source with network calls refused."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tarfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location(
        "asicen_source_audit", Path(__file__).with_name("audit-artifact.py"))
    audit = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(audit)
    result = audit.audit_source_archive(argparse.Namespace(archive=args.archive.resolve()))
    manifest = result["manifest"]
    if not manifest.get("core_dependency"):
        raise RuntimeError("offline rebuild requires bundled Core source")
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=False)
    source = work / "source"
    source.mkdir()
    with tarfile.open(args.archive) as stream:
        stream.extractall(source, filter="data")
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("CMAKE_", "FETCHCONTENT_", "PKG_CONFIG_")):
            del env[key]
    env["PKG_CONFIG_PATH"] = ""

    def run(name: str, command: list[str], cwd: Path) -> None:
        with (work / f"{name}.log").open("w") as log:
            subprocess.run(
                ["strace", "-f", "-e", "trace=network", "-e",
                 "inject=connect:error=ENETUNREACH", "-e",
                 "inject=sendto:error=ENETUNREACH", "-e",
                 "inject=sendmsg:error=ENETUNREACH", "-o", str(work / f"{name}.network.log"),
                 *command], cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)

    libusb_build = work / "libusb-build"
    libusb_build.mkdir()
    libusb_install = work / "libusb-install"
    run("libusb-configure", [str(source / "third_party/libusb-1.0.30/configure"),
                            "--disable-shared", "--enable-static", "--disable-udev",
                            f"--prefix={libusb_install}"], libusb_build)
    run("libusb-build", ["make", "-j2"], libusb_build)
    run("libusb-install", ["make", "install"], libusb_build)
    build = work / "build"
    core = source / "third_party/dtv-userland-core/repository"
    run("configure", ["cmake", "-S", str(source / "repository"), "-B", str(build),
                      "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
                      "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
                      "-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
                      "-DASICEN_BUILD_TESTS=OFF", "-DASICEN_BUILD_PCSC_IFD=OFF",
                      f"-DASICEN_LIBUSB_INCLUDE_DIR={libusb_install}/include/libusb-1.0",
                      f"-DASICEN_LIBUSB_LIBRARY={libusb_install}/lib/libusb-1.0.a"], source)
    run("build", ["cmake", "--build", str(build), "--target", "asicend", "asicen-ts",
                  "asicenctl", "-j2"], source)
    generated = (build / "build.ninja").read_text()
    if str(core) not in generated or str(libusb_install / "lib/libusb-1.0.a") not in generated:
        raise RuntimeError("build did not use bundled Core and rebuilt static libusb")
    version = manifest["version"]
    for program, label in (("asicend", "asicend"), ("asicen-ts", "asicen-ts"),
                           ("asicenctl", "asicen-userland")):
        run(f"{program}-version", [str(build / program), "--version"], source)
        if (work / f"{program}-version.log").read_text() != f"{label} {version}\n":
            raise RuntimeError(f"{program} version differs from the source manifest")
    summary = {"product_commit": manifest["repository_commit"],
               "product_tree": manifest["repository_tree"], "version": version,
               "core": manifest["core_dependency"],
               "libusb_archive_sha256": manifest["libusb_archive_sha256"],
               "core_source_dir": str(core), "network": "connect/sendto/sendmsg refused",
               "programs": ["asicend", "asicen-ts", "asicenctl"]}
    (work / "rebuild-result.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("source archive offline rebuild and three-command version smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
