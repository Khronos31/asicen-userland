#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the packaged IFD prefix against a short-path mock daemon."""
import ctypes
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time


def main() -> int:
    plugin = Path(sys.argv[1])
    daemon_binary = Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="aifd-", dir="/tmp") as temporary:
        root = Path(temporary)
        runtime = root / "r"
        runtime.mkdir(mode=0o700)
        instance = "ifdsmoke"
        daemon_endpoint = runtime / "asicen-userland" / instance / "control.sock"
        proxy_runtime = root / "p"
        proxy_instance = "proxy"
        proxy_endpoint = proxy_runtime / "asicen-userland" / proxy_instance / "control.sock"
        proxy_runtime.mkdir(mode=0o700)
        (proxy_runtime / "asicen-userland").mkdir(mode=0o700)
        proxy_endpoint.parent.mkdir(parents=True, mode=0o700)
        for directory in (proxy_runtime, proxy_runtime / "asicen-userland",
                          proxy_endpoint.parent):
            directory.chmod(0o700)
        byte_counts = {"to-daemon": 0, "from-daemon": 0}
        counts_lock = threading.Lock()
        connection_lock = threading.Lock()
        relay_sockets: list[socket.socket] = []
        stop_proxy = threading.Event()

        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(proxy_endpoint))
        proxy_endpoint.chmod(0o600)
        listener.listen(4)
        listener.settimeout(0.1)

        def relay(client: socket.socket) -> None:
            backend = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                backend.connect(str(daemon_endpoint))
                while True:
                    readable, _, exceptional = select.select([client, backend], [],
                                                               [client, backend], 1.0)
                    if exceptional:
                        return
                    for source in readable:
                        data = source.recv(65536)
                        if not data:
                            return
                        destination = backend if source is client else client
                        destination.sendall(data)
                        key = "to-daemon" if source is client else "from-daemon"
                        with counts_lock:
                            byte_counts[key] += len(data)
            except OSError:
                return
            finally:
                client.close()
                backend.close()

        def accept_clients() -> None:
            while not stop_proxy.is_set():
                try:
                    client, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                with connection_lock:
                    relay_sockets.append(client)
                threading.Thread(target=relay, args=(client,), daemon=True).start()

        proxy_thread = threading.Thread(target=accept_clients, daemon=True)
        proxy_thread.start()
        with (root / "daemon.log").open("wb") as log:
            daemon = subprocess.Popen(
                [str(daemon_binary), "--runtime-dir", str(runtime),
                 "--instance", instance], stdout=subprocess.DEVNULL, stderr=log)
            try:
                deadline = time.monotonic() + 3
                while not daemon_endpoint.exists() and time.monotonic() < deadline:
                    if daemon.poll() is not None:
                        raise AssertionError(f"mock daemon exited: {daemon.returncode}")
                    time.sleep(0.01)
                if not daemon_endpoint.exists():
                    raise AssertionError("mock daemon endpoint did not appear")

                ifd = ctypes.CDLL(str(plugin), mode=getattr(os, "RTLD_NOW", 2))
                create = ifd.IFDHCreateChannelByName
                create.argtypes = [ctypes.c_uint32, ctypes.c_char_p]
                create.restype = ctypes.c_int
                presence = ifd.IFDHICCPresence
                presence.argtypes = [ctypes.c_uint32]
                presence.restype = ctypes.c_int
                close = ifd.IFDHCloseChannel
                close.argtypes = [ctypes.c_uint32]
                close.restype = ctypes.c_int

                device = (f"asicen-userland:runtime={proxy_runtime}:instance={proxy_instance}:"
                          "access=user").encode("utf-8")
                if create(0, device) != 0:
                    raise AssertionError("bundled IFD rejected the ASICEN endpoint")
                # The mock profile intentionally has no card implementation. The
                # presence result is not used as evidence of connectivity: the
                # proxy must observe an actual request and response from the daemon.
                if presence(0) != 616:
                    raise AssertionError("bundled IFD returned an unexpected mock presence")
                if close(0) != 0:
                    raise AssertionError("bundled IFD failed to close its mock channel")
                deadline = time.monotonic() + 2
                while time.monotonic() < deadline:
                    with counts_lock:
                        observed = dict(byte_counts)
                    if observed["to-daemon"] > 0 and observed["from-daemon"] > 0:
                        break
                    time.sleep(0.01)
                if observed["to-daemon"] == 0 or observed["from-daemon"] == 0:
                    raise AssertionError(f"IFD IPC proxy saw no round trip: {observed}")
            finally:
                stop_proxy.set()
                listener.close()
                proxy_thread.join(timeout=1)
                with connection_lock:
                    active_sockets = list(relay_sockets)
                for relay_socket in active_sockets:
                    try:
                        relay_socket.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    relay_socket.close()
                if daemon.poll() is None:
                    daemon.send_signal(signal.SIGTERM)
                    try:
                        daemon.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()
                # Only surface a shutdown defect when the body did not already
                # raise, so the original failure (e.g. IFD rejection) is kept.
                if daemon.returncode != 0 and sys.exc_info()[0] is None:
                    raise AssertionError(f"mock daemon shutdown returned {daemon.returncode}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
