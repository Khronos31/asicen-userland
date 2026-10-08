# Vendored px4-userland source manifest

Source repository: `Khronos31/px4-userland`  
Source revision: `d51c83e1d7eeb829fd61f87f6ea93ad2043b9d00`  
License: GPL-2.0-only; the full license text is `LICENSE`.

All paths below are under this directory. Except for the files listed in
“Local deltas”, the copied files are byte-identical to the source revision.
The upstream checkout was not modified.

```text
LICENSE
userland/include/px4/card.h
userland/include/px4/card_service.h
userland/include/px4/control_client.h
userland/include/px4/control_server.h
userland/include/px4/error.h
userland/include/px4/firmware.h
userland/include/px4/identity.h
userland/include/px4/ipc.h
userland/include/px4/it930x.h
userland/include/px4/posix_ipc.h
userland/include/px4/posix_tuner_nonce.h
userland/include/px4/transport.h
userland/include/px4/tuner_service.h
userland/src/card_service.cpp
userland/src/card.cpp
userland/src/control_client.cpp
userland/src/control_server.cpp
userland/src/control_server_test_access.h
userland/src/control_workers.cpp
userland/src/control_workers.h
userland/src/error.cpp
userland/src/identity.cpp
userland/src/ipc.cpp
userland/src/posix_ipc.cpp
userland/src/posix_ipc_test_access.h
userland/src/posix_tuner_nonce.cpp
userland/src/posix_tuner_nonce_internal.h
userland/src/tuner_service.cpp
userland/tests/card_service_tests.cpp
userland/tests/card_tests.cpp
userland/tests/control_integration_tests.cpp
userland/tests/control_workers_tests.cpp
userland/tests/ipc_state_tests.cpp
userland/tests/posix_ipc_tests.cpp
userland/tests/posix_tuner_nonce_tests.cpp
userland/tests/px4_ts_tests.cpp
userland/tests/px4ctl_format_tests.cpp
userland/tests/px4d_list_format_tests.cpp
userland/tests/test_main.cpp
userland/tests/test_temp_directory.h
userland/tests/tuner_service_tests.cpp
userland/tools/px4_ts.cpp
userland/tools/px4_ts_core.cpp
userland/tools/px4_ts_core.h
userland/tools/px4_ts_posix.cpp
userland/tools/px4_ts_posix.h
userland/tools/px4ctl.cpp
userland/tools/px4ctl_format.cpp
userland/tools/px4ctl_format.h
userland/tools/px4d_list_format.cpp
userland/tools/px4d_list_format.h
```

The build uses the portable sources and the service, card protocol, IPC,
worker, nonce, stream-client, and formatter test suites named in
`docs/cli-adaptation.md`.
`tests/imported_tests_main.cpp` is local glue. Upstream test assertions are
retained; the additional local test is listed with the other deltas below.

## Local deltas

- `userland/include/px4/tuner_service.h`: adds a default no-op
  `TunerServiceBackend::request_stop()` and a forwarding service method.
- `userland/include/px4/card_service.h`: adds default no-op stop notifications
  for the backend and card protocol session. Its native `CardSession` adapter
  is conditionally omitted in this product's portable targets via the
  propagated `PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION` build definition; the
  protocol session interface and `CardService` remain available. The native
  adapter still depends on the excluded It930x card transport.
- `userland/include/px4/card.h`: adds `CardSession::initialize_with_atr()` so a
  hardware shim can validate the ATR profile before beginning T=1 negotiation.
- `userland/src/card.cpp`: retains the upstream CardSession, ATR and T=1 code,
  adds validation for `initialize_with_atr()`, and excludes the
  `It930xCardHardware` forwarding definitions when `ASICEN_PROFILE_ASICEN` or
  `PX4_USERLAND_DISABLE_NATIVE_CARD_SESSION` is defined. ASICEN supplies its
  own mailbox-backed `CardHardware`.
- `userland/tests/card_tests.cpp`: retains the upstream assertions and adds
  focused validation for `initialize_with_atr()`.
- `userland/src/control_server.cpp`: ASICEN receiver-lane dispatch and invokes
  tuner, card, and stream cancellation hooks before draining workers.
- `userland/src/control_workers.cpp`: ASICEN receiver-to-USB-function lane
  split while retaining the unmodified reference split when the profile macro
  is absent.
- `userland/src/ipc.cpp`: ASICEN wire magic, receiver count and USB-function
  inventory under the profile macro.
- `userland/src/posix_ipc.cpp`: ASICEN runtime path and product instance
  validation under the profile macro.
- `userland/tools/px4_ts_core.cpp` and `userland/tools/px4ctl.cpp`: product
  command naming, receiver bounds, serial-free instance routing, no-LNB guard,
  usage and error labels under `ASICEN_PRODUCT_CLI`.

The upstream service tests and applicable CLI tests are compiled without
changing their assertions. Product-specific mapping, shutdown, worker routing,
CLI, and research-daemon lifecycle cases live in `userland/tests/` outside this
vendored snapshot.

## Updating this subset

The ASICEN maintainer reviewing an update must compare each imported file with
the pinned revision using `git show REVISION:PATH`, review upstream IPC,
worker and stream-lifecycle fixes, and reapply the small local deltas above.
Update this manifest and revision together; retain original test assertions
and run both the reference-profile suites and ASICEN lifecycle tests. Do not
copy a newer client or server in isolation without checking their shared IPC
contract. New hardware-specific upstream code is outside this subset.
