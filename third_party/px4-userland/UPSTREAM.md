# Vendored px4-userland source manifest

Source repository: `Khronos31/px4-userland`
Source revision: `1a1485d0c3e972e0a47be907edb67949564aa9a7`
Refreshed for ASICEN issue #4 on 2026-10-10.
License: GPL-2.0-only; the full license text is `LICENSE`.

The upstream checkout is read-only and was not modified. Except for the
explicit local deltas below, imported files are byte-identical to this pin,
including platform helpers, tests, formatting, and final newlines. The retained
`test_main.cpp` is the full reference snapshot for audit provenance; it is not
the selected ASICEN test runner and does not add IT930x hardware to this build.

## Imported files

```text
LICENSE
packaging/pcsc/reader.conf.d/px4-userland.conf.in
userland/include/px4/card.h
userland/include/px4/card_service.h
userland/include/px4/control_client.h
userland/include/px4/control_server.h
userland/include/px4/error.h
userland/include/px4/firmware.h
userland/include/px4/identity.h
userland/include/px4/ipc.h
userland/include/px4/it930x.h
userland/include/px4/logging.h
userland/include/px4/mock_transport.h
userland/include/px4/pcsc_ifd_adapter.h
userland/include/px4/platform_sleep.h
userland/include/px4/posix_ipc.h
userland/include/px4/posix_tuner_nonce.h
userland/include/px4/transport.h
userland/include/px4/tuner_service.h
userland/include/px4/windows_tuner_nonce.h
userland/src/card.cpp
userland/src/card_service.cpp
userland/src/control_client.cpp
userland/src/control_server.cpp
userland/src/control_server_test_access.h
userland/src/control_workers.cpp
userland/src/control_workers.h
userland/src/error.cpp
userland/src/identity.cpp
userland/src/ipc.cpp
userland/src/logging.cpp
userland/src/mock_transport.cpp
userland/src/pcsc_ifd.cpp
userland/src/pcsc_ifd_adapter.cpp
userland/src/posix_ipc.cpp
userland/src/posix_ipc_test_access.h
userland/src/posix_tuner_nonce.cpp
userland/src/posix_tuner_nonce_internal.h
userland/src/tuner_service.cpp
userland/src/windows/windows_ipc.cpp
userland/src/windows/windows_security.h
userland/src/windows/windows_sleep.cpp
userland/src/windows/windows_tuner_nonce.cpp
userland/tests/card_service_tests.cpp
userland/tests/card_tests.cpp
userland/tests/control_integration_tests.cpp
userland/tests/control_workers_tests.cpp
userland/tests/ipc_state_tests.cpp
userland/tests/ipc_tests.cpp
userland/tests/pcsc_ifd_tests.cpp
userland/tests/posix_ipc_tests.cpp
userland/tests/posix_tuner_nonce_tests.cpp
userland/tests/px4_ts_tests.cpp
userland/tests/px4ctl_format_tests.cpp
userland/tests/px4d_list_format_tests.cpp
userland/tests/px4d_signal_tests.cpp
userland/tests/test_main.cpp
userland/tests/test_poll_compat.h
userland/tests/test_temp_directory.h
userland/tests/tuner_service_settle_tests.cpp
userland/tests/tuner_service_tests.cpp
userland/tests/windows_control_tests.cpp
userland/tests/windows_platform_tests.cpp
userland/tests/windows_stdin_monitor_tests.cpp
userland/tests/windows_ts_output_tests.cpp
userland/tests/windows_worker_tests_main.cpp
userland/tools/px4_ts.cpp
userland/tools/px4_ts_core.cpp
userland/tools/px4_ts_core.h
userland/tools/px4_ts_posix.cpp
userland/tools/px4_ts_posix.h
userland/tools/px4_ts_windows.cpp
userland/tools/px4_ts_windows_output.cpp
userland/tools/px4_ts_windows_output.h
userland/tools/px4_windows_args.h
userland/tools/px4ctl.cpp
userland/tools/px4ctl_format.cpp
userland/tools/px4ctl_format.h
userland/tools/px4d_list_format.cpp
userland/tools/px4d_list_format.h
userland/tools/px4d_signals.cpp
userland/tools/px4d_signals.h
userland/tools/px4d_windows_stdin.cpp
userland/tools/px4d_windows_stdin.h
```

## ASICEN-only test glue and regression tests

```text
userland/tests/asicen_firmware_provider_tests.cpp
userland/tests/asicen_ifd_capability_tests.cpp
userland/tests/asicen_ipc_tests_main.cpp
userland/tests/asicen_ts_argument_tests.cpp
userland/tests/identity_endpoint_tests.cpp
userland/tests/imported_tests_main.cpp
userland/tests/windows_imported_tests_main.cpp
```

## Necessary local deltas

- `userland/include/px4/card.h` and `userland/src/card.cpp`: retain the existing
  probe-only `initialize_with_atr()` entry. The ASICEN probe must classify the
  acquired ATR before protocol negotiation (LRC/19200 only), without resetting
  and consuming a second ATR. The live `CardSession::initialize()` body is now
  byte-identical to the pinned method. The additional entry reparses the saved
  bytes, invalidates stale state, and uses the same RESYNCH/IFS setup. IT930x
  forwarding definitions stay excluded from ASICEN/no-native-session targets.
  `card_tests.cpp` retains every original test and adds success, tampered ATR,
  zero/oversize length, reinitialization, exact reset counts and negotiation order.
- `userland/include/px4/card_service.h` and `tuner_service.h`: retain the native
  stop-notification seams and the no-IT930x card-session adapter guard. ASICEN
  adapters use cancellation to release GPIO/card-operation and streaming waits;
  notifications do not perform cleanup. `control_server.cpp` invokes these
  hooks only under `ASICEN_PROFILE_ASICEN`, before the reference listener-close,
  accepted-task/completion drain, join and service cleanup sequence. Reference
  finite-timeout behavior and shutdown order are unchanged. Removing these
  notifications would strand the existing ASICEN cancellation/rollback contract.
- `userland/src/control_server.cpp` and `control_workers.cpp`: ASICEN routes two
  receiver lanes per USB function rather than four, and accepts no worker
  receiver above index 3. The server alone permits an empty observed serial
  because ASICEN has no USB serial descriptor. Size, instance/path, USB mask,
  wire and other input validation stay pinned. `control_workers_tests.cpp`
  preserves every upstream test with only the product lane index adjusted and
  adds all 0/1/2/3/4/5/7/8/255 product-boundary cases.
- `userland/src/ipc.cpp`: ASCN product magic; physical counts 1/2/4; one combined
  receiver for S3U or alternating S/T pairs with two lanes per USB function.
  Only those physical values/table entries differ. Codec validation, wire
  lengths, iteration and state-machine logic stay pinned. `ipc_tests.cpp`
  runs all upstream codec assertions in reference and ASICEN builds with only
  the magic and LIST topology golden fixtures selected by profile.
- `userland/include/px4/posix_ipc.h`, `userland/src/posix_ipc.cpp` and
  `userland/src/windows/windows_ipc.cpp`: `asicen-userland` runtime and lock
  literals, preserving the reference `snprintf` argument form. The additional
  `SerialEndpointLease::acquire_identity()` validates a real, serial-free
  component identity and requests an exclusive lease regardless of endpoint
  instance. It shares the pinned platform lease implementation; numeric-serial
  `acquire()` retains its existing validation and shared/exclusive behavior.
  Lease scope is the validated runtime directory, not a replacement for native
  physical-device ownership across different runtime directories. No serial is
  fabricated. Existing environment, mode/DACL, symlink/reparse, identity, retry,
  noninheritance and cleanup rules remain. `windows_platform_tests.cpp` adds
  identity tests; its firmware fixture uses the ASICEN provider only when the
  explicit native-loader test macro is set, preserving the reference branch.
- `userland/src/pcsc_ifd_adapter.cpp`: device prefix override, model-independent
  `ASICEN card reader via asicend` type and 19200 maximum baud. Capability is
  selected by `ASICEN_PROFILE_ASICEN`, independently from naming. The parser,
  locking, PTS, APDU and error-mapping bodies stay pinned. Product adapter tests
  supplement (do not replace) the original optional pcsc-lite suite.
- `userland/tools/px4_ts_core.cpp` and `px4ctl.cpp`: public product spelling,
  required nonnumeric `--instance`, absent-serial `--device` rejection and TS
  receiver limit 0..3. Shared parser/control/output/usage shape matches the pin.
  No fixed mock-backend labels or text LIST serial postprocessing remain.
  Windows argv conversion follows the pinned implementation. The product builds
  native entry wrappers separately; importing this copy alone is not evidence
  that a native wrapper was updated.
- `userland/tests/imported_tests_main.cpp` and
  `windows_imported_tests_main.cpp` are selected named registries using the
  reference `Test` array/for/PASS/FAIL form. Every prior selected suite is kept;
  the previously omitted complete wire-codec suite is registered. ASICEN-only
  runners exercise product codec/topology, TS options/usage, PC/SC capability,
  identity leases, and native firmware provider policy boundaries.

## Current Windows subset

`NativeHandle`, `PathChar` and worker `WakeHandle` follow the pin end to end.
The imported Windows IPC/security/nonce/sleep, socket wake, TS sink, UTF-8 argv,
console shutdown and stdin helpers are portable OS adapters, not IT930x device
implementations. Their existing LOCALAPPDATA/USERPROFILE policy, private DACL,
random names, bounded socket waits, noninheritance and identity-safe cleanup are
retained. Product path literals differ only where documented above. The pinned
worker-Winsock initialization observation PC-U01 is not silently changed here.

Windows build/offline testing and physical-device validation remain separate;
importing these files does not establish native Windows hardware support.

## Related native adaptation

The N08 portable helpers `logging.h/.cpp` and `mock_transport.h/.cpp` are
byte-identical to the pinned source. Logging is compiled into the portable core;
its helper/test scope matches the reference, which has no production Logger
callers. MockTransport is built only when tests are enabled. The native
`userland/tests/portable_helper_tests.cpp` extracts the reference's complete
`SinkCapture`, `capture_log`, `test_logging`, `test_mock_transport` and
`test_mock_failures_and_bounds` block without modifying its assertions, and adds
severity/sink, FIFO/observation and stream cleanup boundary coverage.

The helper's original stream configuration still validates the reference 0x84
endpoint. That test is explicitly reference-only; it is not an ASICEN transport
fixture. `userland/tests/capture_transport_tests.cpp` instead drives ASICEN's
actual `run_raw_capture()` through its existing `CaptureBackend` seam using
MockTransport bulk reads on each ASICEN endpoint (0x81/0x82). It preserves the
independent completion error and transferred byte count, exercises timeout and
short-read ordering, checks cleanup/error precedence, and leaves ASICEN DSC
control requests and the production libusb backend unchanged. No IT930x command
frames or physical tuner/card operations are involved.

`userland/include/asicen/firmware.h`, `userland/src/firmware.cpp`, and
`userland/src/sha256.h` outside this directory adapt the same pin's immutable
`FirmwareImage`, `Result<FirmwareImage>` provider and exact SHA-256 class.
ASICEN's explicit ModelId chooses its verified loader size/hash; there is no
IT930x scatter interpretation. The fingerprint research wrapper shares the SHA
class. Product loading can finish validation before any USB initialization.

CI exposed one necessary non-hardware portability correction to the copied
provider I/O: libc++ filebuf can turn a failed fread into EOF, so a directory
was reported as rejected firmware rather than INTERNAL. The ASICEN provider
uses 512-byte stdio reads with an explicit ferror check and scoped fclose;
Windows retains strict UTF-8 conversion and wide-path opening. Open failure,
read failure, size/hash rejection and immutable image contracts are unchanged.
The directory and directory-symlink regressions retain INTERNAL/CLI exit 70.
This is a documented shared-code defect correction, not a hardware exemption.

Likewise, duplicate_fd_cloexec outside this subset now implements its generic
POSIX ownership/CLOEXEC contract on macOS as well as Linux/Android. USB-fd
wrapping itself remains Linux/Android-only, with explicit unsupported-platform
tests; this does not add macOS USB-fd acquisition support.

Product and offline mock commands now use this same versioned control/stream
protocol. The separate 24-byte `--socket` daemon/client entry points were removed;
their capture and shutdown scenarios use canonical runtime/instance endpoints.
No legacy raw-path socket API was added to the vendor boundary. The complete
POSIX `SocketStream` implementation remains byte-identical to the pinned source.

## Updating this subset

Compare every imported file with this pin, preserve each documented value or
adapter boundary, and run the reference-profile and ASICEN-profile suites.
Do not replace existing assertions or mock tests with source-copy claims. Do
not mechanically import hardware-specific IT930x, tuner-register or PX4 libusb
backend sources. Record failed/blocked verification separately from passes.
