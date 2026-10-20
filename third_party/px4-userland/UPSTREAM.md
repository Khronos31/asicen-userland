# Vendored px4-userland source manifest

## 2026-10-20: 独立LNBのlocal delta

`tuner_service.h/.cpp`、`control_server.cpp`、`control_workers.h/.cpp` にCoreのtyped独立LNB要求とSTATUS状態を接続するlocal deltaを追加。GPIO/safetyとdaemon wrapperはASICEN固有backendへ転送する。通常TUNEの電圧省略と明示指定を区別し、独立設定はreceiver closeで解除せずdaemon shutdownでOFFにする。共通server/hostのCore集約は後続作業であり、このvendored deltaを最終共通化完了とは扱わない。

## 2026-10-20: 公開ctlの共有Core frontendへの移行

`userland/tools/asicenctl.cpp` は製品profileとinstance名の大文字保持だけを扱う薄い入口とし、parser・runtime探索・LIST整形・STATUS・CARD lifecycleはCoreの `dtv::host_ctl` 静的targetをPX4と共有する。Core revisionは `cmake/dtv-userland-core.revision` に固定する。PX4のfrontend sourceをASICENへ複製・includeする方式は使わない。

共通LISTはPX4側と同じ状態名（例 `ready=ready`）と、不明なserialの `serial=unknown` を使う。ASICENのserial selector非対応、既存instance名の大文字小文字、固有USB/loader/backendは製品側に保持する。既存host transportは引き続き `asicen_portable_core` が供給する。共有frontendはCoreの専用GPL adapterとして分離され、portable MIT targetsへGPLや製品依存を入れない。通常公開CARD操作は既存Core typed clientで送受信する。

Source repository: `Khronos31/px4-userland`
Source release: [v0.2.1](https://github.com/Khronos31/px4-userland/releases/tag/v0.2.1)
Source revision: `dd1c1bea16a5871085d5f2a42c009364c1ec7345`
Annotated tag object: `4939fdec32900a807252586e807e97f19e6b6612`.
Refreshed from the official release for ASICEN 0.1.0 on 2026-10-13.
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
userland/include/px4/ts_continuity.h
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
userland/tests/card_client_contract_tests.h
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
userland/tests/ts_continuity_tests.cpp
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

## Official v0.2.1 refresh

The prior code pin was `1a1485d0c3e972e0a47be907edb67949564aa9a7`.
Comparing all 84 previously imported files, upstream changed 12 and left 72
byte-identical. Seven changed files are refreshed exactly; five retain their
existing ASICEN delta. The two new CARD_HANDLE_STATUS golden frames select
the existing product-magic macro just like all other frames. The shared
checker, its complete standalone fixtures and the shared card-client contract
fixtures are added exactly, giving 87 imported files (70 exact and 17 with the
local deltas below). The imported-file list and necessary local deltas in this
manifest are the current source-reuse record.

The refresh includes opt-in generation-bound card handles, owned handle-status
snapshots, full capability-mask validation and the Windows worker's explicit
runtime-directory propagation. Existing clients do not opt in implicitly.
The common IPC changes are portable; they do not add a WinSCard DLL, change
ASICEN's supported platforms or authorize Windows 0.1.0 release assets.

`ts_continuity.h` is header-only C++17 with only standard-library dependencies.
There is no new common-core library, network build fetch or hardware import.
ASICEN's native HardwareStreamService owns one nothrow heap-allocated checker,
resets it for each accepted capture attempt and maps only `error` into its
continuity counter. Its separate TEI counter, packet publication and queue
contracts remain native. The standalone upstream fixtures and native service
counter/byte-preservation regressions cover this adapter; their presence is not
a statement that the new candidate has passed execution or hardware checks.

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
- `userland/include/px4/error.h`, `logging.h`, `transport.h` and
  `mock_transport.h`: replaced by thin facades that alias `dtv::userland`
  (see the foundation dependency revision below). `userland/include/px4/firmware.h`
  qualifies the `Result` friend declaration as `dtv::userland::Result` so the
  shared `Result` alias still grants `FirmwareImage` access. This is the only
  declaration adjustment; the firmware protocol/asset logic is untouched.

## Common foundation dependency revision 1

2026-10-18 JST. Base ASICEN main `76299585bbaf7dd77e579554565db2b050df4232`.
The seven N08 foundation files (`include/px4/{error,logging,transport,mock_transport}.h`
and `src/{error,logging,mock_transport}.cpp`) were compared against the fixed
PX4 checkout `90f08a594487e2603bd5c09f778a0b3b64ac9a4d`; all seven were
byte-identical to that upstream source, so ASICEN has no vendor delta in them.
The reviewed shared alias/dependency pattern is therefore applied unchanged, and
the canonical algorithms now come from the MIT `dtv-userland-core` commit
`4873d346476e9bac9073bf39ffba807fdc892733`, pinned in
`cmake/dtv-userland-core.revision` and linked as `dtv::core`.

The four headers become aliases and keep only the product device constants and
the PX4-preserving default `MockTransport` policy in the facade. The three
duplicate algorithm sources are no longer vendored: `src/error.cpp`,
`src/logging.cpp` and `src/mock_transport.cpp` are removed from this directory.
The file list above remains the historical import record. Hardware, card,
current IPC/wire byte content and CLI behavior are unchanged; the product
VERSION stays 0.1.0.

Pre-change (pin-identical) and post-change SHA-256:

```text
include/px4/error.h
  pin  aba979d8421e9dab4513715bb4aa89e3c30ecfaee477054b99b974ad95ec0013
  now  669a16caafcfc987c79b0944342dc9beda179ac7f62864be59da2b1fcedd2f25
include/px4/logging.h
  pin  e80b422205205976044328560c7d188dd7c64107fee0bfcde5c3aacf751979d9
  now  73f4f91ac839c6bcf5046df28f787b0c247f006e2c6bdf2eed908366caf20062
include/px4/transport.h
  pin  0a594c7cb64057c66f614e9320df3383c02ad83e1acd69bbe143d01140b5426f
  now  70da3cefa60bf0a85a940c00409c5e6d510499fcc86b21b17c3fc711a2de21f5
include/px4/mock_transport.h
  pin  59adf3ffe5a0dd16df2b6fd3d36c48c92db4c9aee6d45881f21814701985df2a
  now  4b356a87584a4e79562b53eed4b13c8c70dd257952e2ac5e3d13db640e14e1ab
include/px4/firmware.h
  pin  4e455838d09988e5d377bb747278f27a24f673a3f654d5193e46ae17f7572edb
  now  f60c682796145f216004c35a3451f2083f1f5c9e1eafd593ce1ed23386428e4d
src/error.cpp
  pin  0eecdb29a8f8cd7c023f7529638791354f69a732b56418e0ec6b7a02090604dc  (removed)
src/logging.cpp
  pin  8c1ee995fac110304bf2290a91e6c4f405bf17b970ec31dcd4cfc2a800767265  (removed)
src/mock_transport.cpp
  pin  9abadf6337034bc4143f952a8ae583075e9434e01dc0978eb559df78b8e97906  (removed)
```

Two additional Windows-only build corrections are retained: the trailing
backslash comment in `src/windows/windows_ipc.cpp` is rephrased, and
`tests/windows_ts_output_tests.cpp` includes `<cstring>`. Neither changes
executable behavior or test expectations. No other vendored file changed.
The corresponding local-adaptation record for
ASICEN's own `userland/include/asicen/firmware.h` uses the same qualified
`dtv::userland::Result` friend declaration.

## Common receiver engine adoption revision 1

2026-10-18 JST. The common lock is advanced to the reviewed combined MIT
`dtv-userland-core` commit `121071c3458e6a03d69f05794c066b704996b13a`, which
contains the optional `dtv::receiver` target. `userland/include/px4/tuner_service.h` and
`userland/src/tuner_service.cpp` now use the accepted PX4 receiver facade as
the compatibility adapter over that engine. The product facade continues to
own legacy request validation, receiver IDs and active-profile count, timing
policy, and backend type conversion. The engine receives the active profile's
receiver count selected before service construction; hot-add and mutable
backend counts remain unsupported.

ASICEN retains the local `TunerServiceBackend::request_stop()` and
`TunerStreamControl::request_stop()` hooks. `TunerService::request_stop()`
notifies only the backend through the facade adapter. Existing control-server
shutdown separately notifies tuner backend, card backend, and stream control
in its existing order. No stop fan-out was added to the common engine, and
`HardwareStreamService` remains in the ASICEN product layer. The imported
facade remains GPL-2.0-only under the existing product source boundary; the
receiver engine is covered by the common dependency's MIT notice above.

## Common card ownership engine adoption revision 1

2026-10-18 JST. The same reviewed combined dependency commit
`121071c3458e6a03d69f05794c066b704996b13a` adds the optional `dtv::card`
interface target and opaque-ATR `CardOwnershipEngine`. ASICEN's
`userland/include/px4/card_service.h` facade and `userland/src/card_service.cpp`
use the accepted PX4 facade template and engine adapter. The product keeps its
NativeCardProtocolSession adapter behind the existing
`PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION` guard, and `request_stop()` still
notifies the backend before the protocol session without running cleanup.
Card ATR parsing, APDU/T=1, hardware interfaces/defaults, and backend ownership
remain in the GPL product layer. This reuse does not add a copied ownership
engine, new hardware support, public v1/WinSCard behavior, or CLI migration.

The unchanged P card ownership characterization harness was run against the
pre-extraction ASICEN service and the new ASICEN facade. New ASICEN-specific
coverage checks backend-before-session stop notification and deferred shutdown
cleanup. `RESOURCE_LIMIT`, introduced by the combined common error enum but not
represented by the current IPC `ErrorCode`, maps to the existing generic
internal-error paths; existing wire values and product exit codes are unchanged.

## Current Windows subset

`NativeHandle`, `PathChar` and worker `WakeHandle` follow the pin end to end.
The imported Windows IPC/security/nonce/sleep, socket wake, TS sink, UTF-8 argv,
console shutdown and stdin helpers are portable OS adapters, not IT930x device
implementations. Their existing LOCALAPPDATA/USERPROFILE policy, private DACL,
random names, bounded socket waits, noninheritance and identity-safe cleanup are
retained. Product path literals differ only where documented above. The official v0.2.1 worker fix now forwards an explicit UTF-8 runtime parent
through the server into wake-socket creation; only a null parent selects
LOCALAPPDATA. Trust, encoding and length validation remain fail-closed. The
upstream native Windows regression is imported unchanged.

Windows build/offline testing and physical-device validation remain separate;
importing these files does not establish native Windows hardware support.

## Related native adaptation

The N08 portable helpers `logging.h/.cpp` and `mock_transport.h/.cpp` now come
from the MIT `dtv-userland-core` foundation (see the foundation dependency
revision above): the product headers are aliases and the duplicate
implementations are no longer vendored. Logging is compiled into `dtv::core`;
its helper/test scope matches the reference, which has no production Logger
callers. The mock implementation is compiled into a separate object in the
`dtv::core` static archive; the product's mock test targets use that object.
Compiling the archive does not itself mean a normal production link includes
the mock object. The native
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
