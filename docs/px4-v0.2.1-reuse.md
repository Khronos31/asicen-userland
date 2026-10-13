# Official PX4 v0.2.1 source reuse for ASICEN 0.1.0

## Fixed inputs and scope

- ASICEN starting commit: `432684cc5c9919e1e3379d7ba64db7d6ebb7795c` on `release/0.1.0`.
- Previous PX4 source pin: `1a1485d0c3e972e0a47be907edb67949564aa9a7`.
- Official source: [PX4 v0.2.1](https://github.com/Khronos31/px4-userland/releases/tag/v0.2.1), published 2026-10-13 UTC.
- Annotated tag object `4939fdec32900a807252586e807e97f19e6b6612` resolves to full commit `dd1c1bea16a5871085d5f2a42c009364c1ec7345`.
- Product VERSION stays `0.1.0`. These source changes must be built and tested before fixing a new candidate. They are not release notes or release approval.

The import remains a source subset under `third_party/px4-userland/`, not a new externally linked common-core library. All 84 earlier imported files were compared by their fixed Git blob identities. Twelve changed upstream, 72 did not. Three exact upstream files are added, for 87 total. Seventy imports are exact and 17 preserve explicitly recorded local adapters/tests. ASICEN-only test glue is outside this count. The complete inventory and rationale for every retained adapter are in [UPSTREAM.md](../third_party/px4-userland/UPSTREAM.md).

No IT930x backend, tuner register implementation, WinSCard proxy/DLL, PX4 hardware startup policy, or PX4 release asset matrix is imported. Release automation has independent provenance in [its record](release-automation-provenance.md). Its source binding is not silently changed by the C++ pin update.

## Changed source mapping

Paths below are upstream paths; the local destination is always `third_party/px4-userland/<path>`. “Preserve delta” means apply the upstream change to the previous ASICEN copy while retaining its existing product boundary. It does not authorize diverging from the new common contract.

| Upstream path | Import operation | New upstream Git blob |
| --- | --- | --- |
| `userland/include/px4/card_service.h` | Preserve delta | `e6850a782f8e11a89000b722fbe545302a241c13` |
| `userland/include/px4/ipc.h` | Replace exact | `d0fabed1cfb468d01aa91b035144a2a0c43d566a` |
| `userland/src/card_service.cpp` | Replace exact | `d7611932200c3a3282d6b488a0da26fb22fe25af` |
| `userland/src/control_server.cpp` | Preserve delta | `b23e3c4c797a2ee690eccd33c168c938cc5bba40` |
| `userland/src/control_workers.cpp` | Preserve delta | `4d003bbf9e6aa3628f1e000f7c2df85e2574d34d` |
| `userland/src/control_workers.h` | Replace exact | `2c20439b24a022fc71c7513d1bc37445b246efde` |
| `userland/src/ipc.cpp` | Preserve delta | `a94013fa70b0c29b68f07b18ab839c827b05fa8d` |
| `userland/tests/card_service_tests.cpp` | Replace exact | `0f6e7f76f6cd9951193b68d551d5f1645d8036b4` |
| `userland/tests/control_integration_tests.cpp` | Replace exact | `a69cc60c248f8d15aa25b21b7bef990bd4bc50c8` |
| `userland/tests/ipc_state_tests.cpp` | Replace exact | `1fc1500f8b5b51d5495d29c615cfa633ec15caa5` |
| `userland/tests/ipc_tests.cpp` | Preserve delta | `c52f69bcdbe6be45e67c2a110a9abe606055d64a` |
| `userland/tests/windows_control_tests.cpp` | Replace exact | `51330cc45b19030faff798bad1e62c6b871f545e` |
| `userland/include/px4/ts_continuity.h` | Add exact | `79d58a08a4d5fcb15fd46d6fe52d3955df797c1b` |
| `userland/tests/ts_continuity_tests.cpp` | Add exact | `03893ee57b6476f0f9fe86ec207b6b535a13cdb4` |
| `userland/tests/card_client_contract_tests.h` | Add exact | `848d730e4e7f38a6fbae485550a2684760ed4897` |

The five updated local-adapter files are `card_service.h`, `control_server.cpp`, `control_workers.cpp`, `ipc.cpp`, and `ipc_tests.cpp`. Four retain exactly the same old-pin-to-ASICEN textual delta after applying the new upstream hunks. The codec test retains all existing deltas and adds the existing `PX4_TEST_MAGIC` selection to the two new owned-handle-status golden frames, with a dated change notice. Both capability-mask checks are updated; neither side keeps the former any-bit acceptance rule.

The following 12 locally adapted imports had no upstream change and remain untouched:

- `userland/include/px4/card.h`
- `userland/include/px4/posix_ipc.h`
- `userland/include/px4/tuner_service.h`
- `userland/src/card.cpp`
- `userland/src/pcsc_ifd_adapter.cpp`
- `userland/src/posix_ipc.cpp`
- `userland/src/windows/windows_ipc.cpp`
- `userland/tests/card_tests.cpp`
- `userland/tests/control_workers_tests.cpp`
- `userland/tests/windows_platform_tests.cpp`
- `userland/tools/px4_ts_core.cpp`
- `userland/tools/px4ctl.cpp`

Every remaining unchanged import is an exact upstream blob already present at both fixed pins. The mapping does not describe an 84-file rewrite; it verifies the unchanged files while refreshing only the 12 that changed and adding the three dependencies.

## Portable card and runtime changes

The refreshed service/IPC files carry the official optional generation-bound card contract: HELLO capability bit 3, CARD_HANDLE_STATUS 0x0038, full-mask capability enforcement, owned read-only status snapshots, sticky invalidation after observed card loss, and explicit reset/reconnect recovery. Legacy clients remain unbound unless they negotiate the new bit. The upstream common card-client fixture is required by both the POSIX integration and native Windows control tests; it is not a WinSCard test or hardware operation.

The Windows worker fix passes the selected UTF-8 runtime parent from the server to wake-socket creation. Null alone selects ambient LOCALAPPDATA. Existing security/encoding/path-length failures remain failures. Product shutdown notifications, no-native-card-session guards, ASCN wire magic, receiver counts/lane mapping, absent USB serial allowance and other documented ASICEN deltas stay intact.

## Native CC adapter

- New exact upstream header: `userland/include/px4/ts_continuity.h`.
- New exact standalone fixtures: `userland/tests/ts_continuity_tests.cpp`.
- Native integration: `userland/include/asicen/hardware_stream_session.h`, `userland/src/hardware_stream_session.cpp`.
- Native counter/byte/epoch regressions: `userland/tests/hardware_stream_session_tests.cpp`.
- Build registration: `CMakeLists.txt`, standalone `asicen-ts-continuity-tests` on POSIX and Windows.

HardwareStreamService owns one checker on the heap, allocated once with nothrow before capture. The released [upstream checker documentation](https://github.com/Khronos31/px4-userland/blob/dd1c1bea16a5871085d5f2a42c009364c1ec7345/docs/ts-continuity.md) reports 1,638,408 bytes (about 1.56 MiB) on its tested 64-bit ABI. This is upstream size evidence, not a measurement of the ASICEN candidate. That storage is not embedded in stack-allocated services or allocated in the packet callback. A failed allocation returns INTERNAL before source preparation. Every accepted fresh capture attempt resets the checker through the existing clear-attempt boundary.

The existing observation point receives complete transformed packets. Only `TsContinuityKind::error` increments continuity-errors. The independent TEI counter still observes TEI packets. Invalid or TEI packets do not become trusted CC baselines and invalidate duplicate adjacency; a subsequent trusted packet can expose a real gap. Valid adaptation-only packets, current discontinuity indicators, one duplicate pair, PCR-only duplicate exceptions, further repeats and byte conflicts follow the shared fixtures. No malformed-header wire counter is invented.

This changes observation, not the packet stream. Raw byte delivery, ordering, queue/counter timing, source/framer counters, final snapshots and native start/stop/cleanup remain in place. In the old six-packet quality fixture, the lone alleged CC error came from TEI itself; the same bytes correctly yield zero CC errors under the new contract. A separate trusted-gap test must still report a real CC error. Native tests also compare emitted bytes and capture-epoch reset rather than relying on source equality alone.

## Unchanged product and diagnostic boundaries

- ASICEN firmware bytes and their binary-inclusion/source-exclusion policy are unchanged. Existing unresolved redistribution-rights notices remain.
- Windows source/build/offline checks remain; no Windows asset is added to the 0.1.0 eight-binary release set.
- ASICEN USB topology, product identity, encryption/transform, hardware/card/LNB behavior and the existing four-packet sync probe remain unchanged.
- [PR #8](https://github.com/Khronos31/asicen-userland/pull/8) remains a separate unmerged probe-eight diagnostic. [Issue #7](https://github.com/Khronos31/asicen-userland/issues/7) remains unresolved. Lower CC counts do not prove repaired CS2 data or reception quality.
- The separate CLI `--version` request in [Issue #9](https://github.com/Khronos31/asicen-userland/issues/9) is not implemented by this import; official v0.2.1 did not contain it.

## Verification status and required next evidence

This record describes fixed-source inspection and the proposed candidate changes. It claims no build, CTest, native Windows execution, new artifact reproduction or physical hardware pass for the changed candidate. Historical evidence remains attached to its original commit.

Before freezing the candidate, execute the exact shared CC fixtures, native stream/counter/epoch tests, reference and ASICEN codec/worker/card suites, and the full platform CI/packaging/relink matrix. Compare all emitted bytes in the native fixtures. Verify the final artifact source SHA and the unchanged firmware/package policy. The Windows build script already stages both underscore and hyphen test names; the native runner selects `*tests.exe`, so the standalone checker target is included without changing release assets.

Required ASICEN hardware/specification checks and user-selected soak/reproducibility decisions still apply. Release-note authoring and tag/publication remain separate later explicit requests; an offline pass cannot grant either.

## ASICEN receive-performance follow-on (2026-10-13)

After the import above, the ASICEN-specific backend selects four 65,536-byte
bulk-IN transfers instead of four 4,096-byte transfers. The queue depth,
completion/resubmission order, cancellation/drain contract and four-packet
sync probe stay fixed. The size has an official high-throughput source basis in
[linux-abi.md](reverse-engineering/linux-abi.md) and the four pending submissions
in the [guarded trace](hardware-traces/2026-10-08-guarded-init.usbmon.txt).
Neither historical source establishes clean reception for the new candidate.
The lifecycle regression checks the submitted count/size and rejects negative
or requested-length-plus-one completions without resubmission.

The ASICEN mode-7 implementation additionally prepares DES schedules once per
decoder and uses portable fused S/P tables. Packet bytes and per-chunk counters
must match the frozen scalar oracle. The test-only differential/round suites
cover that equivalence; the optional synthetic benchmark is excluded from the
default build and is not a timing-based gate. No timing recorder, raw-input
capture, diagnostic environment variable, workflow adapter or new crypto
dependency is added to production. Shared PX4 sources and their official
`dd1c1bea16a5871085d5f2a42c009364c1ec7345` pin remain untouched.

These follow-on changes supersede only the implementation-level transform/size
description above, not its protocol or support boundaries. They invalidate
earlier candidate binary qualification. Exact changed-candidate build,
reproducibility and the existing eight-archive hardware/soak evidence remain
required by [release-validation.md](release-validation.md) and
[release-record.md](release-record.md); no hardware pass is claimed here.
