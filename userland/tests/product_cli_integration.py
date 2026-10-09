#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Process-level acceptance for the ASICEN mock-only command profile."""

import os
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


DAEMON, CTL, TS = map(Path, sys.argv[1:4])


def run(*args, timeout=5, **kwargs):
    return subprocess.run(args, timeout=timeout, check=False, **kwargs)


def wait_socket(path, proc):
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if path.exists():
            return
        if proc.poll() is not None:
            raise AssertionError(f"daemon exited early: {proc.returncode}")
        time.sleep(0.01)
    raise AssertionError("daemon socket did not appear")


with tempfile.TemporaryDirectory(prefix="asicen-cli-") as temp:
    root = Path(temp)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    instance = "acceptance"
    endpoint_dir = runtime / "asicen-userland" / instance
    endpoint = endpoint_dir / "control.sock"
    log = open(root / "daemon.log", "wb")
    daemon = subprocess.Popen(
        [str(DAEMON), "--mock", "--runtime-dir", str(runtime),
         "--instance", instance], stdout=subprocess.DEVNULL, stderr=log)
    children = [daemon]
    logs = [log]
    try:
        wait_socket(endpoint, daemon)

        listing = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                      instance, "list", capture_output=True, text=True)
        assert listing.returncode == 0, listing.stderr
        assert "serial=null" in listing.stdout and "backend=mock-only" in listing.stdout
        expected = ["receiver=0 device=1 local=0 system=ISDB-S",
                    "receiver=1 device=1 local=1 system=ISDB-T",
                    "receiver=2 device=2 local=0 system=ISDB-S",
                    "receiver=3 device=2 local=1 system=ISDB-T"]
        assert all(line in listing.stdout for line in expected), listing.stdout

        status = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                     instance, "status", capture_output=True, text=True)
        assert status.returncode == 3 and "UNSUPPORTED" in status.stderr
        no_daemon = run(str(CTL), "--runtime-dir", str(root / "missing"),
                        "--instance", instance, "list", capture_output=True, text=True)
        assert no_daemon.returncode == 3 and "NOT_FOUND" in no_daemon.stderr
        card = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                   instance, "card-status", capture_output=True, text=True)
        assert card.returncode == 3 and "UNSUPPORTED" in card.stderr
        unsupported = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                          instance, "card-atr", capture_output=True, text=True)
        assert unsupported.returncode == 3, (unsupported.returncode, unsupported.stderr)

        invalid = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                      instance, "--receiver", "1", "--channel", "T27",
                      "--packet-count", "3", "--duration-seconds", "1",
                      capture_output=True)
        assert invalid.returncode == 2, invalid.returncode

        help_text = run(str(TS), "--help", capture_output=True, text=True)
        assert help_text.returncode == 0 and "--lnb-voltage 0|15" in help_text.stdout
        for value in ("13", "18", "256", "-1", "on", "15V"):
            invalid_lnb = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                              instance, "--receiver", "0", "--channel", "BS01_0",
                              "--lnb-voltage", value, "--packet-count", "1",
                              capture_output=True)
            assert invalid_lnb.returncode == 2, (value, invalid_lnb.stderr)
        for value in ("0", "15"):
            terrestrial_lnb = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                                  instance, "--receiver", "1", "--channel", "T27",
                                  "--lnb-voltage", value, "--packet-count", "1",
                                  capture_output=True)
            assert terrestrial_lnb.returncode == 2, terrestrial_lnb.stderr

        capture = root / "capture.ts"
        finite = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                     instance, "--receiver", "1", "--channel", "T27",
                     "--packet-count", "3", "--output", str(capture),
                     capture_output=True, timeout=5)
        assert finite.returncode == 0, finite.stderr.decode(errors="replace")
        data = capture.read_bytes()
        assert len(data) == 3 * 188, len(data)
        assert all(data[offset] == 0x47 for offset in range(0, len(data), 188))
        assert b"stream packets" not in data

        stdout_capture = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                             instance, "--receiver", "1", "--channel", "T27",
                             "--packet-count", "3", "--output", "-",
                             capture_output=True, timeout=5)
        assert stdout_capture.returncode == 0
        assert len(stdout_capture.stdout) == 3 * 188
        assert all(stdout_capture.stdout[offset] == 0x47
                   for offset in range(0, len(stdout_capture.stdout), 188))
        assert b"stream packets" not in stdout_capture.stdout
        assert b"stream packets" in stdout_capture.stderr

        # A duplicate instance/runtime must not remove the owner's socket.
        duplicate = run(str(DAEMON), "--mock", "--runtime-dir", str(runtime),
                        "--instance", instance, capture_output=True, timeout=3)
        assert duplicate.returncode != 0
        assert endpoint.is_socket(), "duplicate daemon removed the live endpoint"

        # The legacy research mode must also fail safely on the owned path.
        legacy = run(str(DAEMON), "--mock", "--socket", str(endpoint),
                     capture_output=True, timeout=3)
        assert legacy.returncode != 0
        assert endpoint.is_socket(), "research daemon unlinked product endpoint"
        still_live = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                         instance, "list", capture_output=True, text=True)
        assert still_live.returncode == 0 and "serial=null" in still_live.stdout

        # Closing a stream consumer must not wedge either client or daemon.
        consumer = subprocess.Popen(
            [str(TS), "--runtime-dir", str(runtime), "--instance", instance,
             "--receiver", "1", "--channel", "T27", "--packet-count", "100000",
             "--output", "-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        children.append(consumer)
        assert consumer.stdout is not None
        consumer.stdout.close()
        try:
            consumer.wait(timeout=3)
        except subprocess.TimeoutExpired:
            consumer.kill()
            consumer.wait()
            raise AssertionError("client stalled after consumer closed")
        assert daemon.poll() is None

        # A blocked output pipe and partial control request must not hold
        # daemon shutdown. The client is deliberately left blocked on stdout.
        slow_consumer = subprocess.Popen(
            [str(TS), "--runtime-dir", str(runtime), "--instance", instance,
             "--receiver", "1", "--channel", "T27", "--packet-count", "100000",
             "--output", "-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        children.append(slow_consumer)
        time.sleep(0.1)
        assert slow_consumer.poll() is None
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.connect(str(endpoint))
        sock.sendall(b"\x01")
        started = time.monotonic()
        daemon.send_signal(signal.SIGTERM)
        assert daemon.wait(timeout=2) == 0
        assert time.monotonic() - started < 2
        assert not endpoint.exists()
        slow_consumer.terminate()
        try:
            slow_consumer.wait(timeout=1)
        except subprocess.TimeoutExpired:
            slow_consumer.kill()
            slow_consumer.wait()
        sock.close()

        research_socket = root / "research.sock"
        research_log = open(root / "research.log", "wb")
        research = subprocess.Popen(
            [str(DAEMON), "--mock", "--socket", str(research_socket)],
            stdout=subprocess.DEVNULL, stderr=research_log)
        children.append(research)
        logs.append(research_log)
        wait_socket(research_socket, research)
        partial = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        partial.connect(str(research_socket))
        partial.sendall(b"\x01")
        started = time.monotonic()
        research.send_signal(signal.SIGTERM)
        assert research.wait(timeout=2) == 0
        assert time.monotonic() - started < 2
        partial.close()
        research_log.close()
        assert not research_socket.exists()

        research_socket = root / "research-slow.sock"
        research_log = open(root / "research-slow.log", "wb")
        research = subprocess.Popen(
            [str(DAEMON), "--mock", "--socket", str(research_socket)],
            stdout=subprocess.DEVNULL, stderr=research_log)
        children.append(research)
        logs.append(research_log)
        wait_socket(research_socket, research)
        slow_research_client = subprocess.Popen(
            [str(TS), "--socket", str(research_socket), "--receiver", "1",
             "--packet-count", "100000"], stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        children.append(slow_research_client)
        time.sleep(0.1)
        assert slow_research_client.poll() is None
        started = time.monotonic()
        research.send_signal(signal.SIGTERM)
        assert research.wait(timeout=2) == 0
        assert time.monotonic() - started < 2
        slow_research_client.terminate()
        try:
            slow_research_client.wait(timeout=1)
        except subprocess.TimeoutExpired:
            slow_research_client.kill()
            slow_research_client.wait()
        research_log.close()

        # The same upstream LNB contract is simulated for every model when the
        # daemon grants --allow-lnb-power. These are synthetic null-packet
        # captures, not evidence of physical output voltage or reception on any
        # of the model-specific hardware paths.
        for model in ("s3u", "s3u2", "w3u2", "w3u3", "w3u3-v2"):
            model_instance = "lnb-" + model
            model_endpoint = runtime / "asicen-userland" / model_instance / "control.sock"
            model_log = open(root / (model + ".log"), "wb")
            logs.append(model_log)
            model_daemon = subprocess.Popen(
                [str(DAEMON), "--mock", "--model", model, "--allow-lnb-power",
                 "--runtime-dir", str(runtime),
                 "--instance", model_instance], stdout=subprocess.DEVNULL, stderr=model_log)
            children.append(model_daemon)
            wait_socket(model_endpoint, model_daemon)
            for lnb_args in ([], ["--lnb-voltage", "15"], ["--lnb-voltage", "0"]):
                satellite = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                                model_instance, "--receiver", "0", "--channel", "BS01_0",
                                *lnb_args, "--packet-count", "3", "--output", "-",
                                capture_output=True)
                assert satellite.returncode == 0, (model, lnb_args, satellite.stderr)
                assert len(satellite.stdout) == 3 * 188
                assert all(satellite.stdout[offset:offset + 3] == b"\x47\x1f\xff"
                           for offset in range(0, len(satellite.stdout), 188))
            model_daemon.send_signal(signal.SIGTERM)
            assert model_daemon.wait(timeout=2) == 0
            assert not model_endpoint.exists()

        # Without --allow-lnb-power the mock daemon refuses the 15 V request,
        # matching px4's default-off safety gate. Zero volts stays accepted.
        denied_instance = "lnb-denied"
        denied_endpoint = runtime / "asicen-userland" / denied_instance / "control.sock"
        denied_log = open(root / "lnb-denied.log", "wb")
        logs.append(denied_log)
        denied_daemon = subprocess.Popen(
            [str(DAEMON), "--mock", "--model", "w3u3", "--runtime-dir", str(runtime),
             "--instance", denied_instance], stdout=subprocess.DEVNULL, stderr=denied_log)
        children.append(denied_daemon)
        wait_socket(denied_endpoint, denied_daemon)
        denied_on = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                        denied_instance, "--receiver", "0", "--channel", "BS01_0",
                        "--lnb-voltage", "15", "--packet-count", "3", "--output", "-",
                        capture_output=True)
        assert denied_on.returncode == 3 and b"UNSUPPORTED" in denied_on.stderr
        denied_off = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                         denied_instance, "--receiver", "0", "--channel", "BS01_0",
                         "--lnb-voltage", "0", "--packet-count", "3", "--output", "-",
                         capture_output=True)
        assert denied_off.returncode == 0 and len(denied_off.stdout) == 3 * 188
        denied_daemon.send_signal(signal.SIGTERM)
        assert denied_daemon.wait(timeout=2) == 0
        assert not denied_endpoint.exists()
    finally:
        for child in children:
            if child.poll() is None:
                child.send_signal(signal.SIGTERM)
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
        for stream in logs:
            stream.close()

print("ASICEN mock CLI lifecycle acceptance passed")
