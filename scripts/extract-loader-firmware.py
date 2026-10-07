#!/usr/bin/env python3
"""Extract the embedded 16 KiB ASICEN firmware blob from the official loader.ko.

This tool does not download or redistribute firmware. It operates on a loader.ko
provided by the user and writes the embedded FirmBin object to a separate file.
It expects GNU readelf and the historical unstripped PLEX/ASICEN loader module.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import subprocess
import sys


FIRMBIN_SIZE = 0x4000
EXPECTED_START_ADDR = 0x5399


def run(*args: str) -> str:
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def parse_sections(loader: pathlib.Path) -> dict[int, tuple[int, int, str]]:
    text = run("readelf", "-SW", str(loader))
    sections: dict[int, tuple[int, int, str]] = {}
    pattern = re.compile(
        r"^\s*\[\s*(\d+)\]\s+(\S+)\s+\S+\s+"
        r"([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+)"
    )
    for line in text.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        index = int(match.group(1))
        name = match.group(2)
        address = int(match.group(3), 16)
        offset = int(match.group(4), 16)
        size = int(match.group(5), 16)
        sections[index] = (address, offset, name)
        # Section size is not needed for extraction; symbol size is authoritative.
        _ = size
    return sections


def parse_symbols(loader: pathlib.Path) -> dict[str, tuple[int, int, int]]:
    text = run("readelf", "-sW", str(loader))
    symbols: dict[str, tuple[int, int, int]] = {}
    pattern = re.compile(
        r"^\s*\d+:\s+([0-9a-fA-F]+)\s+(\d+)\s+"
        r"\S+\s+\S+\s+\S+\s+(\d+)\s+(\S+)$"
    )
    for line in text.splitlines():
        match = pattern.match(line)
        if not match:
            continue
        value = int(match.group(1), 16)
        size = int(match.group(2))
        section = int(match.group(3))
        name = match.group(4)
        symbols[name] = (value, size, section)
    return symbols


def symbol_bytes(
    image: bytes,
    sections: dict[int, tuple[int, int, str]],
    symbol: tuple[int, int, int],
) -> bytes:
    value, size, section_index = symbol
    if section_index not in sections:
        raise RuntimeError(f"symbol section {section_index} not found")
    section_address, section_offset, _ = sections[section_index]
    file_offset = section_offset + (value - section_address)
    end = file_offset + size
    if file_offset < 0 or end > len(image):
        raise RuntimeError("symbol range falls outside ELF file")
    return image[file_offset:end]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("loader_ko", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()

    image = args.loader_ko.read_bytes()
    sections = parse_sections(args.loader_ko)
    symbols = parse_symbols(args.loader_ko)

    for required in ("FirmBin", "FirmwareStartAddr"):
        if required not in symbols:
            raise RuntimeError(f"{required} symbol not found; is this the official unstripped loader.ko?")

    firm = symbol_bytes(image, sections, symbols["FirmBin"])
    if len(firm) != FIRMBIN_SIZE:
        raise RuntimeError(f"unexpected FirmBin size: {len(firm)} (expected {FIRMBIN_SIZE})")

    start_raw = symbol_bytes(image, sections, symbols["FirmwareStartAddr"])
    if len(start_raw) != 2:
        raise RuntimeError(f"unexpected FirmwareStartAddr size: {len(start_raw)}")
    start_addr = struct.unpack("<H", start_raw)[0]
    if start_addr != EXPECTED_START_ADDR:
        raise RuntimeError(
            f"unexpected firmware start address 0x{start_addr:04x}; "
            f"expected 0x{EXPECTED_START_ADDR:04x}"
        )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(firm)

    print(f"wrote {len(firm)} bytes to {args.output}")
    print(f"firmware_start_address=0x{start_addr:04x}")
    print("stage1 offset=0x0000 length=0x0c00 final_request=0xab")
    print("stage2 offset=0x2000 length=0x0400 final_request=0xab")
    print("stage3 offset=0x2800 length=0x1000 final_request=0xab")
    print("stage4 offset=0x3800 length=0x0800 final_request=0xac")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
