#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract a release inventory of static archive members from an Android LLD map."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys


ARCHIVE_MEMBER = re.compile(r"(?P<archive>[^\s()]+\.a)\((?P<member>[^()\r\n]+)\)")
NDK_ARCHIVES = {
    "libandroid_support.a",
    "libc++_static.a",
    "libc++abi.a",
    "libunwind.a",
}


def category(archive: str) -> str:
    name = pathlib.PurePosixPath(archive).name
    if name == "libusb-1.0.a":
        return "libusb"
    if name in NDK_ARCHIVES or name.startswith("libclang_rt."):
        return "ndk-runtime"
    if "/CMakeFiles/" in archive or archive.endswith("libasicen_userland_core.a"):
        return "asicen-userland"
    return "other-static"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--map", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--require-libusb", action="store_true")
    args = parser.parse_args()

    text = args.map.read_text(encoding="utf-8", errors="strict")
    members: set[tuple[str, str, str]] = set()
    archive_names: set[str] = set()
    for match in ARCHIVE_MEMBER.finditer(text):
        archive_path = match.group("archive")
        archive_name = pathlib.PurePosixPath(archive_path).name
        member = match.group("member").strip()
        if (not archive_name or not member or "\t" in member or "\\" in member or
                member.startswith("/") or ".." in pathlib.PurePosixPath(member).parts):
            raise ValueError(f"invalid archive member in {args.map}")
        archive_names.add(archive_name)
        members.add((category(archive_path), archive_name, member))

    if not members:
        raise ValueError(f"no static archive members found in {args.map}")
    if "libc++_static.a" not in archive_names:
        raise ValueError(f"libc++_static.a members missing from {args.map}")
    if args.require_libusb and "libusb-1.0.a" not in archive_names:
        raise ValueError(f"libusb-1.0.a members missing from {args.map}")
    if not args.require_libusb and "libusb-1.0.a" in archive_names:
        raise ValueError(f"unexpected libusb-1.0.a members in IPC client {args.map}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("category\tarchive\tmember\n")
        for row in sorted(members):
            output.write("\t".join(row) + "\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, UnicodeError, ValueError) as error:
        print(f"android-link-inventory: {error}", file=sys.stderr)
        raise SystemExit(1)
