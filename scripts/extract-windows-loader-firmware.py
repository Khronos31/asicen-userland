#!/usr/bin/env python3
"""Recover the verified shared Windows loader image from a user-supplied SYS.

This performs static data decoding, never loads or executes vendor code, and
never downloads a driver. The accepted x64 BDA loader is byte-identical in
PLEX's W3U2 1.0.3, W3U3 1.1 and W3U3 V2 1.0 packages. The decoding key is read
from that verified input; neither keys nor firmware are embedded in this tool.
Requires the Python cryptography package for standard AES-128-ECB decoding.

Evidence addresses use the PE preferred virtual addresses: encoded image
17460..1b45f; byte decrement/AES block loop 10f09..10f81; mode-1 decryption
dispatch 128d0 -> 12534; command index at 17450; transfer calls 10fc4..1105d.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import sys


LOADER_SIZE = 59904
LOADER_SHA256 = "edd9badb7588f59c6438e1ceaa0e0f3f33b2ce75667e541e51d75c45e2c485e5"
FIRMWARE_SIZE = 16384
FIRMWARE_SHA256 = "5db39facda140bdcb3a238dad10da5aac752a1ba3aaf17e2239f8d79a0ad6a2a"
START_ADDRESS = 0x55D6


def pe_reader(image: bytes):
    """Return a bounded virtual-address reader for the hash-verified PE image."""
    header = struct.unpack_from("<I", image, 0x3C)[0]
    if image[:2] != b"MZ" or image[header:header + 4] != b"PE\0\0":
        raise ValueError("input is not a PE image")
    machine, count = struct.unpack_from("<HH", image, header + 4)
    optional_size = struct.unpack_from("<H", image, header + 20)[0]
    optional = header + 24
    if machine != 0x8664 or struct.unpack_from("<H", image, optional)[0] != 0x20B:
        raise ValueError("expected the verified x64 PE32+ loader")
    image_base = struct.unpack_from("<Q", image, optional + 24)[0]
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        _, rva, size, raw = struct.unpack_from("<IIII", image, offset + 8)
        sections.append((image_base + rva, size, raw))

    def read(address: int, length: int) -> bytes:
        for base, size, raw in sections:
            if base <= address and address + length <= base + size:
                offset = raw + address - base
                result = image[offset:offset + length]
                if len(result) == length:
                    return result
        raise ValueError("required data falls outside a file-backed PE section")

    return read


def reconstruct(image: bytes) -> bytes:
    if len(image) != LOADER_SIZE or hashlib.sha256(image).hexdigest() != LOADER_SHA256:
        raise ValueError("loader size or SHA-256 does not match the verified Windows input")
    read = pe_reader(image)
    if int.from_bytes(read(0x17450, 2), "little") != START_ADDRESS:
        raise ValueError("unexpected firmware command index")

    # Sixteen immediate stack-byte stores initialize the block-decoding key.
    # Validate each instruction and its stack destination; never log the bytes.
    key = bytearray(16)
    for index in range(len(key)):
        instruction = read(0x1055F + index * 8, 8)
        expected = b"\xc6\x84\x24" + struct.pack("<I", 0x368 + index)
        if instruction[:7] != expected:
            raise ValueError("unexpected static key-initialization instruction")
        key[index] = instruction[7]

    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError as exc:
        raise ValueError("Python cryptography is required for static AES decoding") from exc

    encoded = read(0x17460, FIRMWARE_SIZE)
    ciphertext = bytes((value - 1) & 0xFF for value in encoded)
    cipher = Cipher(algorithms.AES(bytes(key)), modes.ECB())
    decoder = cipher.decryptor()
    payload = decoder.update(ciphertext) + decoder.finalize()
    encoder = cipher.encryptor()
    if encoder.update(payload) + encoder.finalize() != ciphertext:
        raise ValueError("AES decoding failed its complete round-trip check")
    key[:] = bytes(len(key))

    if len(payload) != FIRMWARE_SIZE or hashlib.sha256(payload).hexdigest() != FIRMWARE_SHA256:
        raise ValueError("decoded firmware does not match the verified payload fingerprint")
    for offset in (0x36E6, 0x3728):
        descriptor = payload[offset:offset + 18]
        if (descriptor[:2] != b"\x12\x01" or
                int.from_bytes(descriptor[2:4], "little") != 0x0200 or
                int.from_bytes(descriptor[8:10], "little") != 0x1738 or
                int.from_bytes(descriptor[10:12], "little") != 0x1234 or
                descriptor[7] != 64 or descriptor[17] != 1):
            raise ValueError("decoded image has unexpected USB device descriptors")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("loader_sys", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    try:
        payload = reconstruct(args.loader_sys.read_bytes())
        # Refuse to silently replace an existing image or follow a destination
        # symlink. Keeping firmware outside the source checkout is recommended.
        with args.output.open("xb") as destination:
            destination.write(payload)
        print(f"wrote {len(payload)} bytes to {args.output}")
        print(f"sha256={FIRMWARE_SHA256}")
        print(f"firmware_start_address=0x{START_ADDRESS:04x}")
        print("ranges=0000+0c00,2000+0400,2800+1000,3800+0800")
    except (OSError, ValueError, struct.error) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
