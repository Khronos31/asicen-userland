# Autonomous continuation after the official TS capture

Starting point: `06ca460`. Objective: replace the remaining official-SDK
receive dependency with portable ASICEN code, and deliver the px4-userland
command workflow with honest hardware/unsupported status.

## Constraints and prior art

The user authorizes work that requires no physical operation or new personal
decision. Keep LNB writes disabled, no GPIOEx writes, no hub reset, no VM or HA
restart, no release/version bump/publication. Preserve raw evidence and existing
tests. Read-only reference: px4-userland `d51c83e1d7eeb829fd61f87f6ea93ad2043b9d00`.
Do not change that repository. Existing SPEC.md prior-art evaluation remains
applicable: adapt its portable service/CLI components; its IT930x backend is
not an ASICEN replacement. Official kernel/library use stays an isolated
reference experiment, not the product runtime.

## Increments and acceptance

1. Source-backed output preparation/transport transform: determine which
   GenEncSeed writes actually occur and which per-packet operation the Key2
   read path performs. Record byte order and state dependencies. Any portable
   transform must pass deterministic synthetic/differential tests against the
   inspected official routine, without distributing proprietary binary code or
   captured keys. Static addresses alone do not establish receive correctness.
2. Direct libusb diagnostic: build and run existing offline tests; apply only
   source-supported preparation to the identified receiver, with bounded capture
   and cleanup. Analyze output with `validate_capture_ts.py`, require valid
   PAT/PMT CRC and record every integrity failure. A lack of access or required
   physical recovery is a blocker for hardware verification, not offline work.
3. CLI compatibility: document exact px4 command/option/output/error mapping
   and the reuse dependency closure before integration. Mechanical offline
   tests cover help, invalid arguments, endpoint ownership, busy leases,
   duration/count/output, authoritative terminal errors, signals, and consumers
   that close or stop reading. Retain research tests unchanged. Product code
   must not depend on the legacy vendor archive or CentOS guest.
4. Card/recisdb: inspect the available protocol and services read-only; proceed
   only with proven card transport and bounded owner-safe access. Clear-stream
   decode remains unverified until an actual legitimate-card trial passes.

Baseline commands: `cmake -S . -B build -DASICEN_ENABLE_LIBUSB=ON`,
`cmake --build build --parallel 2`, `ctest --test-dir build --output-on-failure`.
New increments add focused tests without editing existing expectations.

The previous independent critique still requires enclosure-wide ownership,
joined workers, cancellable bounded I/O and terminal stream status before
hardware is attached to a daemon. Use red-team review before committing to a
new service architecture. Do not merely extend the unsafe research daemon.

Rollback: retain small commits on the private branch; revert the affected
increment rather than resetting unrelated work. Stop only tracked task PIDs,
release USB handles and restore saved non-LNB state. Physical recovery, if
needed, is reported to the user. Preserve unsuccessful traces as evidence.

Cheaper alternative: reuse px4's established portable clients/service rather
than duplicating their lifecycle. Hidden premises: the official read path
still has integrity failures, and GenEncSeed's individual necessary device
writes are not isolated. Source rollback is established by clean starting
commit; hardware cold recovery is still unverified.
