# px4-userland CLI adaptation boundary

Current compatibility verdict: see the
[2026-10-09 audit](compatibility-audit-2026-10-09.md).
This historical increment record does not establish distribution parity.
Primary satellite and live internal-card use were added after its original
hardware section; see the [live trial](reverse-engineering/live-card-stream-trial-2026-10-09.md).

Reference revision: `Khronos31/px4-userland`
`d51c83e1d7eeb829fd61f87f6ea93ad2043b9d00` (local source inspected).
This document records the portable CLI increment and its original review.
An experimental hardware backend now passes the receiver1/T27 daemon trial
described below; broader receiver/channel and card support remain unfinished.

## Hardware increment, 2026-10-09

`asicend --hardware` takes explicit `--primary`, `--primary-port`, `--sibling`
and `--sibling-port` targets. It owns both USB functions and the enclosure lock
before exposing the imported service. Only receiver1/T27 is enabled. The
frontend performs shared initialization and one source-backed RF gain step;
the capture worker prepares a fresh private link seed, transforms bulk data
and supplies the existing px4-derived stream client through a bounded queue.

Real hardware verified repeated 30,000-packet captures, stdout, duration,
duplicate-daemon rejection and SIGTERM cleanup. Some captures retain startup
continuity errors. Quality counters in terminal status remain unmeasured;
offline validation is authoritative. Card operations are unsupported, and
hardware enumeration via daemon `--list` is not implemented. The explicit
address/path interface supersedes the proposed `--usb-path` interface below.
The mock-only sections below describe the earlier implementation and gates,
not the present hardware availability. See the [measured results](reverse-engineering/daemon-ts-trial-2026-10-09.md).

## Reuse decision under review

Adapt the portable px4 control/stream client, IPC service, bounded worker
management and endpoint primitives. Keep ASICEN USB, frontend, transport
transform and card-mailbox implementations behind its backend interfaces.
Do not import px4d's IT930x-specific startup, frontend or hardware code.

Dependency closure identified: `error`, `ipc`, `posix_ipc`, `control_client`,
`control_server`, `control_workers`, `posix_tuner_nonce`, `tuner_service`,
`card_service`, and client tools `px4ctl`, `px4ctl_format`, `px4_ts`,
`px4_ts_core`, `px4_ts_posix`, plus referenced headers. `card.h` currently
includes IT930x header types; those declarations do not justify linking an
IT930x backend. Prefer retaining a documented header-only dependency over
rewriting the card protocol prematurely. Source reuse must retain GPL-2.0-only
notices and an exact file/revision manifest. Existing GPL-2.0-or-later files
keep their notices. recisdb is a separate executable, never linked here.

## User-visible contract

| Command | Compatible behavior | ASICEN difference |
| --- | --- | --- |
| `asicend` | `--runtime-dir`, `--instance`, listing, foreground lifecycle | USB topology identity; no invented serial; LNB enable unsupported |
| `asicenctl` | `list`, `status`, `card-status`, `card-atr`, `card-reset`, `card-apdu`; formatting/error conventions | Unimplemented card operations explicitly fail; never fabricate ATR |
| `asicen-ts` | channel/frequency/slot, duration/count, file/stdout, terminal counters and exit codes | Four receiver map `0:S,1:T,2:S,3:T`; unavailable receivers fail |

Use `--instance TOKEN` as the fully compatible client selection path.
The ASICEN hardware has no serial descriptor. Define daemon selection by
observed `--usb-path BUS-PORT` / topology rather than accepting a fictitious
px4 serial. `--list-json` must expose `serial:null` and the observed topology.
Do not claim `--device BASE_SERIAL` compatibility.

Retain the existing explicit `--mock --socket PATH` research workflow and
its tests without silently changing its framing; it must remain isolated
from the hardware backend. The new product IPC uses its own runtime directory
`asicen-userland` and magic, with a documented local adaptation for receiver
descriptors and USB-present mask (two USB functions, two receivers each).
px4 wire-protocol compatibility is not promised by CLI compatibility.

`--channel T27` means557142kHz. Satellite user input is px4 IF kHz; any
conversion to ASICEN RF tables is backend-internal. T13..T62, BS odd01..23
with slot/TSID, CS even2..24 and parsing rules follow the reference client.
Use the actual reference spelling `--tune-timeout-ms` (100..30000), not an
invented tune `--timeout-ms` alias. `--duration-seconds` and `--packet-count`
are mutually exclusive; stdout contains only TS when selected as output.

Exit codes:0 success,2 usage,3 not found/not ready/unsupported,4 busy,
5 timeout,6 IPC/version/protocol,7 USB/disconnect,8 TS integrity/backpressure,
9 card,10 firmware,70 internal/output. Reading the requested byte count is
not sufficient for success: the authoritative terminal result must be checked.

## Acceptance before connecting hardware

- Existing17tests remain green and unchanged.
- New isolated mock service covers list/status, T/S receiver mapping, finite
  file/stdout capture, invalid arguments, no-daemon and busy failures.
- Duplicate daemon must fail without unlinking another endpoint. A partial
  request and slow/closed stream consumer must not prevent bounded shutdown.
- SIGTERM/SIGINT stop and join all workers before destroying backends.
- USB ownership is enclosure-wide, shared with direct diagnostic tools and
  independent of the user-selected runtime/instance. Endpoint locks alone do
  not fulfill this condition. Physical access stays disabled until verified.
- Normal mocks and hardware limitations remain explicit in stderr/status;
  mock packet counts do not establish reception or card readiness.

Cheaper alternatives considered: keeping the research24-byte IPC would require
reimplementing terminal status, ownership, bounded I/O and service lifetime;
copying all of px4d would bring unrelated IT930x/MLT hardware assumptions.
Reusing the selected portable dependency closure avoids both changes.

## Independent design review and required revisions

The independent review returned REVISE with four objections:

1. Receiver descriptor changes alone miss worker routing: both the worker
   validator and server dispatcher currently use receiver<4 for USB1. For
   ASICEN the split is receiver<2. A blocked receiver0 must not block receiver2,
   while receiver1 remains serialized with0; both USB mask bits must round-trip.
2. Joined threads are not a bounded shutdown guarantee: queued30-second tunes
   can accumulate. Before shutdown, the ASICEN backend must receive a stop
   request; running operations observe it and queued operations fail promptly.
   The mock acceptance budget is2seconds with delayed tuner/card/stream
   operations and populated queues. Hardware requires a separately verified
   cancellation/deadline budget before attachment. No pthread force-cancel.
3. The legacy research daemon can unlink the product socket and use freed
   backend pointers despite never opening USB. Fix its active-path protection
   and join/drain lifetime; test targeting the product endpoint, partial requests
   and a blocked consumer. Merely naming it research does not isolate it.
4. Importing lifecycle code without its regression tests creates an unverified
   safety fork. Preserve applicable upstream test sources, record any profile
   distinction, and add ASICEN mapping tests. Maintain an upstream-revision/file
   manifest and local delta; review later upstream lifecycle fixes against this
   baseline before updating. Do not weaken reference expectations to get green.

These objections are accepted. The first product service is mock-only until
these checks pass; hardware ownership remains an explicit later gate. Local
profile differences must be centralized where possible rather than scattering
replacement topology constants. CLI source reuse remains reversible and the
reference repository is unchanged. The user has authorized autonomous work
within these constraints; no release or production deployment is included.

The accepted revisions govern the mock implementation; attaching hardware is
a later, separately tested increment within the same authorized scope.

## Implemented mock increment

`asicend`, `asicenctl`, and `asicen-ts` are now the product target names.
`asicend --mock` starts the imported IPC service with the ASICEN profile;
`--list` and `--list-json` return an empty mock-only inventory with
`serial:null`. `--usb-path` is rejected while hardware support is disabled.
The service exposes four receivers in the mapping `0:S, 1:T, 2:S, 3:T` and
routes receiver 0/1 work to USB-function lane 0 and 2/3 work to lane 1.
`asicen-ts` accepts the reference channel/frequency, stream ID/slot,
bandwidth, tune timeout, duration, packet-count and output options. Mock TS
packets are synthetic null packets; their count is not evidence of reception.

The mock has no card detector or card protocol backend. `status`,
`card-status`, `card-atr`, `card-reset`, and `card-apdu` therefore report
`UNSUPPORTED` (exit 3) where card state is required; they do not claim that a
physical card is absent and do not fabricate ATR/APDU data. `list` remains
available without card state. The selected mock daemon lock in `/tmp` exists
only to test duplicate mock instances. It is not shared with USB tools and
does not satisfy hardware enclosure ownership.

The explicit `asicend --mock --socket PATH` and `asicen-ts --socket PATH`
research workflow remains available. The research daemon no longer removes a
pre-existing socket, removes only the inode it created, bounds client I/O,
limits active clients, interrupts sockets at shutdown, and joins client
threads before backend destruction. The product test starts a client that
disconnects its output and one that leaves a partial control frame before
SIGTERM; both daemon lifecycle paths are bounded in the mock suite.

## Vendored reference and local deltas

The selected reference files under `third_party/px4-userland/userland/` come
from revision `d51c83e1d7eeb829fd61f87f6ea93ad2043b9d00`; `third_party/px4-userland/LICENSE`
is copied from that revision. All imported source and test files retain their
SPDX headers. The files used in the build are:

- `include/px4/{card,card_service,control_client,control_server,error,firmware,identity,ipc,it930x,posix_ipc,posix_tuner_nonce,transport,tuner_service}.h`;
- `src/{card_service,control_client,control_server,control_workers,error,identity,ipc,posix_ipc,posix_tuner_nonce,tuner_service}.cpp`, plus `src/control_workers.h`, `src/control_server_test_access.h`, `src/posix_ipc_test_access.h`, and `src/posix_tuner_nonce_internal.h`;
- `tools/px4_ts.cpp`, `tools/px4_ts_core.cpp`, `tools/px4_ts_core.h`, `tools/px4_ts_posix.cpp`, `tools/px4_ts_posix.h`, `tools/px4ctl.cpp`, `tools/px4ctl_format.cpp`, `tools/px4ctl_format.h`, `tools/px4d_list_format.cpp`, and `tools/px4d_list_format.h`;
- unchanged upstream tests `card_service_tests.cpp`, `control_integration_tests.cpp`, `control_workers_tests.cpp`, `ipc_state_tests.cpp`, `posix_ipc_tests.cpp`, `posix_tuner_nonce_tests.cpp`, `px4_ts_tests.cpp`, `px4ctl_format_tests.cpp`, `px4d_list_format_tests.cpp`, and `tuner_service_tests.cpp`, plus `tests/test_temp_directory.h`.

The vendored snapshot also retains the upstream command entrypoints and test
runner sources for review. The compiled upstream test files are byte-identical
to the named revision. The local `tests/imported_tests_main.cpp` only calls
those test entry points; it does not edit or weaken their expectations.
`px4_portable_reference` builds the reference behavior without product profile
defines. `asicen_px4_mock` builds the same service sources with the local
mapping and magic. Product targets compile the local `userland/tools/asicend.cpp`,
`asicend_research.cpp`, `asicenctl.cpp`, `asicen_ts.cpp`,
`asicen_ts_research.cpp`, and `asicen_ts_portable.cpp`, plus imported
`px4_ts_core.cpp`, `px4_ts_posix.cpp`, and formatting code as appropriate.

The copied `src/ipc.cpp`, `src/posix_ipc.cpp`, `src/control_server.cpp`,
`src/control_workers.cpp`, and `include/px4/tuner_service.h` carry the product
profile and/or shutdown-hook deltas. `include/px4/card_service.h` adds default
no-op stop notifications for cancellable card backend/session calls. The
copied `tools/px4ctl.cpp` and `tools/px4_ts_core.cpp` add product-specific
identity validation, help/error labels, receiver bounds, and the no-LNB
product CLI guard; default reference-profile behavior remains compiled without
`ASICEN_PRODUCT_CLI`. These deltas are isolated behind profile/CLI macros
where applicable. The local mock backends and product tool entry points live
under `userland/`; the upstream checkout was not changed.

The product lifecycle tests cover profile mapping, USB-function worker lane
separation, an active and queued same-lane IPC acquire interrupted by shutdown
while the other lane completes, delayed tuner/card/session/stream test doubles,
duplicate daemon endpoint preservation, finite output length, unavailable-
daemon and unsupported-card errors, closed and unread output consumers, and
partial-request SIGTERM cleanup in both product and research daemon modes.
All tests are offline. They do not validate USB exclusivity,
physical reset/tune behavior, stream reception, card status, or hardware
shutdown deadlines.


## Model-specific topology update (0.1.0 preparation)

The ASICEN-only IPC profile now permits canonical one-combined, two-split and
four-split receiver layouts, without changing wire structures or the imported
PX4 reference build. `asicend --mock --model MODEL` exercises each physical
profile. Hardware LIST advertises the operational subset: one receiver for
S3U, two for S3U2 and the primary pair of a four-receiver enclosure. The catalogue
retains physical capacity separately. Count2 is paired with receiver-bearing
USB mask01, not physical reservation mask03. `HardwareStreamService` delegates
capabilities to its frontend rather than imposing a W3U3 receiver map.

The implementation source and hardware-verification boundary are described in
[model-support.md](model-support.md). No mock pass is a hardware-support claim.
