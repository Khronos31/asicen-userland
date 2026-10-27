#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Process-level acceptance for the ASICEN mock-only command profile."""

import os
import json
import signal
import socket
import subprocess
import sys
import tempfile
import time
from contextlib import contextmanager
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


def report_early_exit(phase, name, proc, stderr_path, returncode):
    print(
        f"failure diagnostic phase={phase} monotonic_ns={time.monotonic_ns()} "
        f"process={name} pid={proc.pid} returncode={returncode}",
        file=sys.stderr,
    )
    try:
        stderr = Path(stderr_path).read_text(errors="replace")
    except OSError as error:
        stderr = f"<stderr unavailable: {error}>"
    print(f"stderr[{name}]:\n{stderr}", file=sys.stderr)
    sys.stderr.flush()


@contextmanager
def temporary_runtime_root():
    # Match the pinned C++ fixture policy: keep TMPDIR when the complete
    # endpoint fits, otherwise use /tmp. macOS has the smallest supported
    # sun_path (104 bytes); partial-request is this fixture's longest instance.
    configured_root = os.environ.get("TMPDIR", "/tmp")
    if not os.path.isabs(configured_root):
        configured_root = "/tmp"
    with tempfile.TemporaryDirectory(prefix="asicen-cli-", dir=configured_root) as temp:
        longest = Path(temp) / "runtime/asicen-userland/v1/partial-request/control.sock"
        if len(os.fsencode(longest)) + 1 < 104:
            yield temp
            return
    with tempfile.TemporaryDirectory(prefix="asicen-cli-", dir="/tmp") as temp:
        yield temp


with temporary_runtime_root() as temp:
    root = Path(temp)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    instance = "acceptance"
    endpoint_dir = runtime / "asicen-userland" / "v1" / instance
    endpoint = endpoint_dir / "control.sock"
    log = open(root / "daemon.log", "wb")
    daemon = subprocess.Popen(
        [str(DAEMON), "--runtime-dir", str(runtime),
         "--instance", instance], stdout=subprocess.DEVNULL, stderr=log)
    children = [daemon]
    logs = [log]
    try:
        wait_socket(endpoint, daemon)

        # Mock instances represent independent synthetic enclosures. A shared
        # runtime root must not turn their endpoint ownership into one global
        # hardware lock.
        independent_log = open(root / "independent.log", "wb")
        logs.append(independent_log)
        independent = subprocess.Popen(
            [str(DAEMON), "--runtime-dir", str(runtime), "--instance", "independent"],
            stdout=subprocess.DEVNULL, stderr=independent_log)
        children.append(independent)
        independent_endpoint = runtime / "asicen-userland" / "v1" / "independent" / "control.sock"
        wait_socket(independent_endpoint, independent)
        independent.send_signal(signal.SIGHUP)
        assert independent.wait(timeout=2) == 0
        assert not independent_endpoint.exists()
        assert daemon.poll() is None, (daemon.returncode, (root / "daemon.log").read_text())

        # The obsolete 24-byte protocol is no longer a second command mode.
        # Reject its selector even when leading, without creating any path.
        obsolete_endpoint = root / "obsolete.sock"
        for command in (DAEMON, TS):
            obsolete = run(str(command), "--socket", str(obsolete_endpoint),
                           capture_output=True, text=True)
            assert obsolete.returncode == 2, obsolete.stderr
            assert "argument error:" in obsolete.stderr, obsolete.stderr
            assert not obsolete_endpoint.exists()

        # An obsolete selector in an option value is also a usage error.
        wrong_dispatch = run(str(TS), "--output", "--socket", "--help",
                             capture_output=True, text=True)
        assert wrong_dispatch.returncode == 2
        assert "argument error:" in wrong_dispatch.stderr, wrong_dispatch.stderr
        wrong_daemon_dispatch = run(str(DAEMON), "--runtime-dir", "--socket",
                                    "--instance", "test", capture_output=True, text=True)
        assert wrong_daemon_dispatch.returncode == 2
        assert "argument error:" in wrong_daemon_dispatch.stderr, wrong_daemon_dispatch.stderr

        ctl_help = run(str(CTL), "--help", capture_output=True, text=True)
        assert ctl_help.returncode == 0 and "[--group]" in ctl_help.stdout

        listing = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                      instance, "list", capture_output=True, text=True)
        assert listing.returncode == 0, listing.stderr
        assert listing.stdout.startswith("ready=1 usb-functions=2 receivers=4 readers=1\n"), listing.stdout
        assert "backend=mock-only" not in listing.stdout, listing.stdout
        expected = ["function=1 usb=0b06:0005 present=yes",
                    "function=2 usb=0b06:0005 present=yes"]
        assert all(line in listing.stdout for line in expected), listing.stdout

        status = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                     instance, "status", capture_output=True, text=True)
        assert status.returncode == 0, status.stderr
        status_json = json.loads(status.stdout)
        assert status_json["context"]["state"] == "ready"
        assert len(status_json["receivers"]) == 4
        no_daemon = run(str(CTL), "--runtime-dir", str(root / "missing"),
                        "--instance", instance, "list", capture_output=True, text=True)
        assert no_daemon.returncode == 3 and \
            "runtime namespace is missing" in no_daemon.stderr and \
            "check --runtime-dir and --instance" in no_daemon.stderr, \
            (no_daemon.returncode, no_daemon.stderr)
        card = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                   instance, "card-status", capture_output=True, text=True)
        assert card.returncode == 3 and "remote=11" in card.stderr
        unsupported = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                          instance, "card-atr", capture_output=True, text=True)
        assert unsupported.returncode == 3 and "remote=11" in unsupported.stderr, \
            (unsupported.returncode, unsupported.stderr)

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
        duplicate = run(str(DAEMON), "--runtime-dir", str(runtime),
                        "--instance", instance, capture_output=True, timeout=3)
        assert duplicate.returncode != 0
        assert endpoint.is_socket(), "duplicate daemon removed the live endpoint"

        # The rejected legacy selector cannot remove an owned product path.
        legacy = run(str(DAEMON), "--socket", str(endpoint),
                     capture_output=True, timeout=3)
        assert legacy.returncode == 2
        assert endpoint.is_socket(), "rejected legacy selector unlinked product endpoint"
        still_live = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                         instance, "list", capture_output=True, text=True)
        assert still_live.returncode == 0 and still_live.stdout.startswith("ready=1 ")

        # Closing a stream consumer must not wedge either client or daemon.
        consumer_log = open(root / "consumer.stderr", "wb")
        logs.append(consumer_log)
        consumer = subprocess.Popen(
            [str(TS), "--runtime-dir", str(runtime), "--instance", instance,
             "--receiver", "1", "--channel", "T27", "--packet-count", "100000",
             "--output", "-"], stdout=subprocess.PIPE, stderr=consumer_log)
        children.append(consumer)
        assert consumer.stdout is not None
        consumer.stdout.close()
        try:
            consumer.wait(timeout=3)
        except subprocess.TimeoutExpired:
            consumer.kill()
            consumer.wait()
            raise AssertionError("client stalled after consumer closed")
        daemon_returncode = daemon.poll()
        if daemon_returncode is not None:
            report_early_exit("closed-consumer", "daemon", daemon,
                              root / "daemon.log", daemon_returncode)
            report_early_exit("closed-consumer", "consumer", consumer,
                              root / "consumer.stderr", consumer.poll())
        assert daemon_returncode is None

        # A blocked output pipe and partial control request must not hold
        # daemon shutdown. The client is deliberately left blocked on stdout.
        slow_consumer_log = open(root / "slow-consumer.stderr", "wb")
        logs.append(slow_consumer_log)
        slow_consumer = subprocess.Popen(
            [str(TS), "--runtime-dir", str(runtime), "--instance", instance,
             "--receiver", "1", "--channel", "T27", "--packet-count", "100000",
             "--output", "-"], stdout=subprocess.PIPE, stderr=slow_consumer_log)
        children.append(slow_consumer)
        time.sleep(0.1)
        slow_consumer_returncode = slow_consumer.poll()
        if slow_consumer_returncode is not None:
            report_early_exit("blocked-output-pipe", "slow_consumer", slow_consumer,
                              root / "slow-consumer.stderr", slow_consumer_returncode)
            report_early_exit("blocked-output-pipe", "daemon", daemon,
                              root / "daemon.log", daemon.poll())
        assert slow_consumer_returncode is None
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

        research_instance = "partial-request"
        research_socket = runtime / "asicen-userland" / "v1" / research_instance / "control.sock"
        research_log = open(root / "research.log", "wb")
        research = subprocess.Popen(
            [str(DAEMON), "--runtime-dir", str(runtime), "--instance", research_instance],
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

        research_instance = "blocked-output"
        research_socket = runtime / "asicen-userland" / "v1" / research_instance / "control.sock"
        research_log = open(root / "research-slow.log", "wb")
        research = subprocess.Popen(
            [str(DAEMON), "--runtime-dir", str(runtime), "--instance", research_instance],
            stdout=subprocess.DEVNULL, stderr=research_log)
        children.append(research)
        logs.append(research_log)
        wait_socket(research_socket, research)
        slow_research_client = subprocess.Popen(
            [str(TS), "--runtime-dir", str(runtime), "--instance", research_instance,
             "--receiver", "1", "--channel", "T27", "--packet-count", "100000",
             "--output", "-"], stdout=subprocess.PIPE,
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

        # Mock LNB control follows the source-backed product capability.
        # These are synthetic null-packet captures, not evidence of physical
        # output voltage or reception on any model-specific hardware path.
        for model in ("s3u", "s3u2", "w3u2", "w3u3", "w3u3-v2"):
            model_instance = "lnb-" + model
            model_endpoint = runtime / "asicen-userland" / "v1" / model_instance / "control.sock"
            model_log = open(root / (model + ".log"), "wb")
            logs.append(model_log)
            model_daemon = subprocess.Popen(
                [str(DAEMON), "--model", model, "--allow-lnb-power",
                 "--runtime-dir", str(runtime),
                 "--instance", model_instance], stdout=subprocess.DEVNULL, stderr=model_log)
            children.append(model_daemon)
            wait_socket(model_endpoint, model_daemon)
            for lnb_args in ([], ["--lnb-voltage", "15"], ["--lnb-voltage", "0"]):
                satellite = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                                model_instance, "--receiver", "0", "--channel", "BS01_0",
                                *lnb_args, "--packet-count", "3", "--output", "-",
                                capture_output=True)
                if model in ("s3u", "s3u2") and lnb_args == ["--lnb-voltage", "15"]:
                    assert satellite.returncode == 3 and b"UNSUPPORTED" in satellite.stderr, \
                        (model, lnb_args, satellite.stderr)
                    assert satellite.stdout == b""
                    continue
                assert satellite.returncode == 0, (model, lnb_args, satellite.stderr)
                assert len(satellite.stdout) == 3 * 188
                assert all(satellite.stdout[offset:offset + 3] == b"\x47\x1f\xff"
                           for offset in range(0, len(satellite.stdout), 188))
            # Independent ctl LNB request: only models whose list advertises LNB
            # control accept it; the others report UNSUPPORTED (exit 3).
            lnb_on = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                         model_instance, "lnb-on", capture_output=True, text=True)
            if model in ("s3u", "s3u2"):
                assert lnb_on.returncode == 3 and "unsupported" in lnb_on.stderr, \
                    (model, lnb_on.returncode, lnb_on.stderr)
            else:
                assert lnb_on.returncode == 0 and lnb_on.stdout == "lnb_15v_enabled=true\n", \
                    (model, lnb_on.returncode, lnb_on.stdout, lnb_on.stderr)
                on_status = json.loads(run(str(CTL), "--runtime-dir", str(runtime),
                                           "--instance", model_instance, "status",
                                           capture_output=True, text=True).stdout)
                assert on_status["enclosure"]["lnb_15v_enabled"] is True, on_status
                for lnb_args in ([], ["--lnb-voltage", "0"]):
                    capture = run(str(TS), "--runtime-dir", str(runtime), "--instance",
                                  model_instance, "--receiver", "0", "--channel", "BS01_0",
                                  *lnb_args, "--packet-count", "3", "--output", "-",
                                  capture_output=True)
                    assert capture.returncode == 0 and len(capture.stdout) == 3 * 188, \
                        (model, lnb_args, capture.returncode, capture.stderr)
                    after_stop = json.loads(run(str(CTL), "--runtime-dir", str(runtime),
                                                "--instance", model_instance, "status",
                                                capture_output=True, text=True).stdout)
                    assert after_stop["enclosure"]["lnb_15v_enabled"] is True, after_stop
                lnb_off = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                              model_instance, "lnb-off", capture_output=True, text=True)
                assert lnb_off.returncode == 0 and lnb_off.stdout == "lnb_15v_enabled=false\n", \
                    (model, lnb_off.returncode, lnb_off.stdout, lnb_off.stderr)
                off_status = json.loads(run(str(CTL), "--runtime-dir", str(runtime),
                                            "--instance", model_instance, "status",
                                            capture_output=True, text=True).stdout)
                assert off_status["enclosure"]["lnb_15v_enabled"] is False, off_status
            model_daemon.send_signal(signal.SIGTERM)
            assert model_daemon.wait(timeout=2) == 0
            assert not model_endpoint.exists()

        # Without --allow-lnb-power the mock daemon refuses the 15 V request,
        # matching px4's default-off safety gate. Zero volts stays accepted.
        denied_instance = "lnb-denied"
        denied_endpoint = runtime / "asicen-userland" / "v1" / denied_instance / "control.sock"
        denied_log = open(root / "lnb-denied.log", "wb")
        logs.append(denied_log)
        denied_daemon = subprocess.Popen(
            [str(DAEMON), "--model", "w3u3", "--runtime-dir", str(runtime),
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
        denied_ctl_on = run(str(CTL), "--runtime-dir", str(runtime), "--instance",
                            denied_instance, "lnb-on", capture_output=True, text=True)
        assert denied_ctl_on.returncode == 3 and "remote=11" in denied_ctl_on.stderr, \
            (denied_ctl_on.returncode, denied_ctl_on.stderr)
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
