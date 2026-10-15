# Bounded native USB pumping

The capture run caller retains ownership of ordered USB completion delivery,
copying and resubmission. A single NativeThread consumer performs decoding and
the existing packet sink calls. Neither USB/state nor queue locks are held
through libusb calls, decoding, emit callbacks, or thread joins. The consumer
is joined before run returns and before the decoder or device is torn down.

USB geometry remains four 65536-byte requests. Software storage is independently
bounded to 32 blocks of 65536 bytes (2 MiB payload storage plus bookkeeping).
A block held by the consumer or reserved by the producer is occupied; dequeue
does not make it reusable. The producer never waits for free space. Exhaustion
fails capture visibly. Thirty-two is a bounded margin, not a measured minimum
or a guarantee against arbitrary downstream stalls.

Cancellation and sink refusal stop between packets. Queued copied input can be
discarded on cancellation/overflow, with explicit final input/discard accounting.
Input counted as passed to the decoder can contain an un-emitted tail when a
sink stops mid-block; these counters do not claim all copied bytes reached the
client. Ordinary terminal failures can finish preceding copied FIFO input;
concurrent cancellation still wins the stop predicate. Allocation/thread-start
failure follows the existing bounded stop/drain cleanup. Malformed callbacks
and real pre-drain status failures remain latched across transfer release.

Synchronous libusb control transfers can dispatch bulk callbacks on another
thread. The capture event wait rechecks its predicates while holding libusb's
event or event-waiter coordination lock; a check made only before entering
libusb could miss already-ready work. This correction preserves the shared
context and does not add a second USB event thread.

One final stderr summary reports pool high-water, overflow, copied/decoder-input/
discard counts and accounting reconciliation. No payload, hashes, detailed
timing ring, periodic register reads or new recovery operations are added.
The summary is emitted before stop/drain and final callback-error precedence
checks. Its terminal field describes the pump/consumer exit; the returned
CaptureRunResult and client terminal status remain authoritative if cleanup
or a late callback subsequently fails.
The existing optional startup metadata remains unchanged in scope, with its
framing observations now made by the serialized consumer.

## Evidence boundary

Integration base is candidate9151c2a5abba5fc28051564f2ff912c1881f7596.
The implementation is extracted from the separately reviewed lean64 private
candidate. Its W3U3 CS10 5/10/20-minute runs did not reproduce the observed
errors; this is bounded evidence, not proof of a universal cause or cure.
This stripped integration is a new build and does not inherit hardware
qualification automatically. Existing shared backend model support remains
unchanged. No W3U2-specific work or new release/support claim is included.

Tests cover bounded backlog, fatal capacity exhaustion, ordered byte-exact FIFO
reuse, short/zero/invalid completions, cancellation, submit/event/disconnect
errors, thread-start failure and joined cleanup. Platform and hardware evidence
must remain separate from offline fake-USB coverage.
