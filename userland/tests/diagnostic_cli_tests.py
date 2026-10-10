#!/usr/bin/env python3
"""Offline diagnostic parser/output/EOF regression. Never opens USB."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command, expected):
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)
    if result.returncode != expected:
        raise AssertionError((command, result.returncode, expected, result.stderr.decode(errors="replace")))
    return result


def main():
    frontend, probe, transform = map(str, map(Path, sys.argv[1:4]))
    for tool in (frontend, probe, transform):
        run([tool, "--help"], 0)
        run([tool, "--help", "--invalid"], 2)
    with tempfile.TemporaryDirectory(prefix="asicen-diagnostic-") as directory:
        root = Path(directory)
        seed, raw, output = root / "seed", root / "raw", root / "output"
        seed.write_bytes(bytes(16))
        seed.chmod(0o600)
        firmware = root / "missing-firmware"
        card = [probe, "card-probe", "--base", "1-2.1", "--firmware", str(firmware), "--detect"]
        terrestrial = [frontend, "frontend-probe", "--base", "1-2.1", "--firmware", str(firmware),
                       "--device", "1", "--receiver", "1", "--frequency-khz", "557142",
                       "--tune-terrestrial"]
        loader = [probe, "--device", "1:2", "--model", "w3u3", "--firmware", str(firmware),
                  "load-firmware"]
        for malformed in (None, b"", bytes(0x3FFF), bytes(0x4000), bytes(0x4001)):
            if malformed is not None:
                firmware.write_bytes(malformed)
            run(card, 3)
            run(terrestrial, 4)
            run(loader, 3 if malformed is None else 10)
        run(loader + ["--firmware", "duplicate"], 2)
        directory_loader = loader.copy()
        directory_loader[6] = str(root)
        run(directory_loader, 70)
        directory_link = root / "directory-link"
        directory_link.symlink_to(root, target_is_directory=True)
        directory_loader[6] = str(directory_link)
        result = run(directory_loader, 70)
        assert result.stdout == b""
        assert b"INTERNAL" in result.stderr
        args = [transform, "--seed-file", str(seed), "--input", str(raw), "--output", str(output)]
        packet = bytes([0x47, 0x1F, 0xFF, 0x10]) + bytes(184)
        for malformed in (b"", b"garbage", packet, packet * 8 + b"tail"):
            raw.write_bytes(malformed)
            result = run(args, 1)
            assert b"pending_at_eof=" in result.stderr
        raw.write_bytes(packet * 8)
        run(args, 0)
        assert output.stat().st_size == len(packet) * 8
        run(args + ["--output", str(output)], 2)
        run([transform, "--seed-file", str(seed), "--input", str(raw), "--output", str(raw)], 1)
        assert raw.read_bytes() == packet * 8
        run([transform, "--seed-file", str(seed), "--input", str(raw), "--output", str(seed)], 1)
        assert seed.read_bytes() == bytes(16)
        output.unlink()
        output.symlink_to(raw)
        run(args, 1)
        assert raw.read_bytes() == packet * 8
        with open(os.devnull, "rb") as unwritable:
            result = subprocess.run([frontend, "--help"], stdout=unwritable, timeout=5)
            assert result.returncode == 8


if __name__ == "__main__":
    main()
