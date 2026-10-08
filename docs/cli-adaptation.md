# px4-userland CLI adaptation boundary

Reference revision: `Khronos31/px4-userland`
`d51c83e1d7eeb829fd61f87f6ea93ad2043b9d00` (local source inspected).
This document specifies the next implementation; it is not a claim that
the current mock commands already implement it.

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
