# Shared TS continuity checker

`third_party/px4-userland/userland/include/px4/ts_continuity.h` is the standalone,
header-only C++17 component from the pinned [PX4 v0.2.1 source](../third_party/px4-userland/UPSTREAM.md)
under GPL-2.0-only. It includes only standard C++ headers. No PX4 transport,
hardware startup policy, vendor runtime, thread, clock, or library link is needed.

## API and ownership

Create one `px4::userland::TsContinuityChecker` per TS stream before processing
packets. Pass one complete packet to `observe(const std::uint8_t*, std::size_t)`.
The returned `TsContinuityResult` contains a `TsContinuityKind`, PID, CC, payload
presence, and current discontinuity marker. `reset()` starts a fresh epoch.
If an upstream transport drops an untrusted packet without exposing its bytes,
call `observe_untrusted_packet()` to invalidate duplicate adjacency only.
Metadata is usable for trusted packet kinds; a TEI or invalid result must be
handled before relying on its header fields. Only `error` is a CC error.

The checker performs no allocations and never retains the caller's pointer. It
copies the most recent trusted packet for each PID, so byte equality does not
depend on a hash collision assumption. On the tested 64-bit ABI, storage is
1,638,408 bytes per checker (about 1.56 MiB). This is the upstream ABI measurement,
not a measurement of every ASICEN target.
Allocate it on the heap or as part of a heap-owned stream object, not on the
default Windows thread stack. `reset()` clears small validity state without a
large aggregate temporary.
Work per packet is bounded; a global epoch invalidates
duplicate adjacency in constant time after untrusted input. Its theoretical
64-bit wrap uses one bounded PID-table pass, without allocation.

## Packet classification

- The first trusted packet of a non-null PID returns `initial`. Payload advances
  CC modulo 16. Adaptation-only retains CC. A mismatch returns `error` and the
  current trusted packet becomes the next baseline.
- One consecutive same-PID duplicate payload packet returns `duplicate`; the
  third and every additional same-CC payload return `error`. Other PIDs may occur
  between the pair. A same-PID adaptation-only packet breaks duplicate eligibility.
  Conflicting same-CC bytes are errors and do not reopen the duplicate allowance.
- A duplicate must match every byte except the 42 PCR timestamp bits. The PCR
  reserved bits must be ones and the extension must be 0–299. OPCR, flags,
  scrambling/PUSI, stuffing, and payload remain byte-exact. PCR timing itself is
  not validated.
- A valid current discontinuity marker returns `discontinuity` and rebases CC,
  including in adaptation-only packets. The next unflagged packet is checked
  immediately against that baseline.
- Null PID returns `null_packet` without changing any PID state.
- TEI returns `transport_error`. Bad size, sync, AFC, adaptation length, or PCR
  encoding returns `invalid_packet`. Neither advances a trusted CC baseline nor
  counts that packet itself as a CC error. Both invalidate duplicate adjacency
  for all PIDs, since even the untrusted packet's PID cannot be assumed correct.
  A later trusted packet is compared with the last trusted CC; it may reveal a
  gap, or a same-CC conflict whose adjacency can no longer be established.

This is a bounded CC observer, not a complete MPEG-TS conformance checker. It
does not interpret PMT/PCR_PID, validate PCR clock accuracy/new time bases,
enforce the marker-count and one-discontinuity constraints over a continuous DI
state, or validate all optional adaptation, PES, or PSI syntax. It accepts each
valid current DI marker. Null packets are excluded before adaptation validation;
TEI is reported before trusting any further header syntax.

The normative basis is [ITU-T H.222.0 (03/2017), clauses 2.4.3.3–2.4.3.5](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.222.0-201703-S%21%21PDF-E&lang=s&type=items),
printed pages 26–30. PCR encoding is also described in
[H.222.0 (02/2000)](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-H.222.0-200002-S%21%21PDF-E&lang=e&type=items),
clauses 2.1.46 and 2.4.3.4–2.4.3.5. The treatment of corrupted input is an explicit
checker policy, not a recovery algorithm prescribed by those clauses.

## ASICEN integration and reuse

`HardwareStreamService` owns one checker on the heap, allocated once with
nothrow before capture. Allocation failure returns INTERNAL before source
preparation. Each accepted fresh capture attempt resets it at the existing
clear-attempt boundary. Observation receives complete transformed packets;
only `TsContinuityKind::error` increments continuity-errors. The independent
TEI counter remains, and no malformed-header wire counter is added.

This observes quality without changing packet bytes or ordering. ASICEN's
USB pumping, transform, framing, publication, queueing and startup policy are
separate from the shared checker. PX4's IT930x startup withholding and fixed
eight-session storage policy are not imported. Native service tests cover
counter mapping, byte preservation and capture-epoch reset.

The header and standalone fixtures are exact copies of the official pinned
release. There is no runtime PX4 dependency, network fetch or `FetchContent`.

## Offline regression tests

The CMake target/CTest name is `asicen-ts-continuity-tests`, on POSIX and Windows.
The fixture source also builds independently:

```sh
c++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -fno-exceptions -fno-rtti \
  -Ithird_party/px4-userland/userland/include \
  third_party/px4-userland/userland/tests/ts_continuity_tests.cpp -o ts-continuity-tests
./ts-continuity-tests
```

Fixtures cover every initial/wrap CC, real gaps, duplicate pairs/long runs,
interleaved PIDs, same-CC byte conflicts, PCR exceptions and corrupt encodings,
adaptation-only retention/errors/reset markers, malformed/short packets, TEI,
null PID, and epoch reset. The native hardware-stream-session suite additionally verifies ASICEN's
public counters, unchanged packet bytes and capture-epoch reset.
These are offline tests and do not establish reception quality or hardware
qualification. The existing release hardware and user-selected soak gates apply.
