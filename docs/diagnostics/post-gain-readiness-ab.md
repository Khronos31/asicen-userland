# W3U3 terrestrial startup: post-gain readiness A/B diagnostic

This is an opt-in investigation build, not a release candidate or an established
hardware fix. It is based on `a092650dee1e851150a15259d36377b56d12ff86`; that
commit only records results after candidate `b64577e94389fe479d573eed57c745e2eb5573ef`.
The pinned PX4 v0.2.1 source is unchanged.

## Evidence and hypothesis

[Issue #7's E02 report](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6149730821)
places all 79 reported continuity/duplicate-conflict events within saved TS packet
indices 2–178. The quoted sub-0.017-second position is an average-rate estimate,
not a USB or wall-clock timestamp. Later packets were clean in these five short
captures. This does not identify the cause or certify a long run.

The original W3 terrestrial `LibusbW3u3Hardware::is_locked` reads demod B0,
performs FC0012 gain adjustment on its first locked observation, then returns the
observation made before that adjustment. The shared `TunerService` settle window
starts at lock polling, not at gain completion. It therefore does not guarantee
a post-gain locked observation or a full post-gain interval before capture.

This build tests that readiness difference only. It does not prove the gain
operation causes the observed errors. 340 ms is a diagnostic choice derived from
the existing settle interval, not a newly established vendor requirement.

## Modes

Set the environment variable on the **daemon process**, before it starts:

- Unset: original return behavior, no diagnostic event recording.
- `ASICEN_DIAGNOSTIC_POST_GAIN_MS=0`: A; original return behavior and hardware
  sequence, with bounded diagnostic recording.
- `ASICEN_DIAGNOSTIC_POST_GAIN_MS=340`: B; the gain call returns not-ready. A later
  locked B0 observation starts a 340-ms window, and a locked observation at or
  after its end permits acquisition. Any observed unlock resets this window.
  This proves sampled lock continuity only, not continuous observation between
  polls. The existing outer tune deadline and cancellation remain in force.
- Other values fail backend claim with `INVALID_ARGUMENT`. This is not a promise
  of zero device activity before failure: daemon firmware preparation precedes
  backend claim. Do not use invalid values for a no-I/O probe.

The changed readiness path is restricted to the original W3U3/W3U2 terrestrial
frontend. Satellite, S3U/S3U2 and W3U3 V2 paths do not acquire this gate. No new
support or validation claims are made for any model.

No additional filter reset, PID change, output setting, buffer-size change,
TS discard, CC tolerance, or shared PX4 behavior change is introduced. The
existing reset/filter and acquisition sequence runs after readiness succeeds.
An issued reset does not by itself prove every device FIFO is empty.

## Minimal E02 comparison

Use one verified Linux musl x86_64 diagnostic archive for **both** A and B, with
the exact same daemon and client hashes. Keep the existing E02 test harness,
device/port selection, frontend, coax, USB topology, firmware, validator and
timeouts. Do not silently replace it with an E03-host trial.

1. Coordinate any reconnection of the W3U3 with the owner. Confirm its current
   primary/sibling identity; do not reuse historical USB addresses. Finish any
   other tuner activity first. Begin in warm runtime, as in the failing E02 runs.
2. Run five A/B pairs, alternating A then B, each with a fresh daemon and one
   10-second T27 capture on terrestrial receiver 1. Keep the original start,
   stop and inter-run handling. The sole group-setting difference is daemon
   `ASICEN_DIAGNOSTIC_POST_GAIN_MS=0` versus `340`.
3. Capture the daemon's stderr directly into a regular per-run log file. Do not
   stream it through a potentially blocking live consumer. Trace output is
   deferred until capture drain, receiver close, retune or backend destruction.
4. Keep all TS packets, including the beginning. Use the existing full-file
   validation and preserve failure exit codes. Never omit leading packets or
   suppress CC errors to turn a failing run into a pass.
5. Save per-run pre/post status, client stderr/exit status, daemon log/exit status,
   TS size/hash and validation JSON. Summarize CC/duplicate/TEI/sync/queue/USB
   counts, first/last event packet indices, and startup timing below. Publish
   summaries, not TS contents, seeds, ATR or APDU contents.

Do not add `--lnb-voltage` to the terrestrial command or `--allow-lnb-power` to
the daemon. This comparison requires no satellite reception, card transaction,
electrical measurement, power-cycle experiment or long soak. Stop on disconnect,
failed cleanup, crash, zero-byte capture or unexpected device selection; retain
that failure rather than continuing blindly.

## Trace interpretation

Lines prefixed `asicen-startup-diagnostic` give relative monotonic microseconds
from that tune's first recorded event. Up to 256 events are held in memory;
repetitive lock/not-ready events stop after slot 192 to reserve 64 milestone
slots. A `dropped` count reports omitted events. The trace is deliberately bounded.

- `tune_begin` / `tune_complete`: requested frequency/timeout budget and tune completion.
- `pre_gain_lock` / `post_gain_lock`: B0 and its low-nibble lock interpretation.
- `gain_begin` / `gain_complete` / `gain_error`: original gain operation.
- `readiness`: returned B readiness and configured interval. A uses its original
  pre-gain return and therefore does not produce this gate event.
- `filter_reset_begin` / `filter_reset_complete`: existing stream setup.
- `initial_urbs_submitted`: all initial requests submitted; queue depth/chunk size.
- `dsc_started`, `cf40_started`, `controller_output_enabled`: completion of the
  existing start operations, not proof of data arriving at the host.
- `worker_run`: capture worker starts consuming completed slots.
- `delivery`: first four submission-order slots consumed by the worker; fields
  are submission order and actual length. This is **delivery time**, not the USB
  callback timestamp or the time the hardware generated each packet.
- `framing_emitted`: cumulative decoded packet count and sync-search bytes
  discarded by the existing framer after that delivery. No new discard is added.
- `drain_complete`: existing bounded stop/drain outcome.

Compare gain-complete → first post-gain observation → readiness → filter reset →
DSC/CF start → first delivery. Check whether each failed run's event packet
indices lie in its first delivered output, using cumulative emitted counts.
Use post-gain observation timestamps only when present: a saturated trace may
omit them, and an omitted observation is not evidence that no read occurred.
Do not convert delivery timestamps into exact on-air error times.

## Decision

- If A reproduces and B is consistently clean, readiness timing becomes a stronger
  candidate. Repeat/inspect before promoting a product fix; this does not isolate
  gain itself from other elapsed-time effects.
- If both reproduce, keep the diagnostic evidence and investigate device-side
  buffering/output-start and transfer contents/order without hiding the TS prefix.
- If neither reproduces, the comparison is inconclusive; do not declare fixed.
- Any B timeout is a failed readiness attempt, not a successful capture or a
  reason to extend the product deadline automatically.

Changes are confined to this diagnostic branch. Do not merge or tag it as a
release merely because offline tests or these short trials succeed.

## Offline verification before publication

On 2026-10-13 UTC, the Latitude build executor completed:

```sh
cmake -S . -B build-postgain -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DASICEN_BUILD_TESTS=ON -DASICEN_ENABLE_LIBUSB=ON
cmake --build build-postgain -j 4
ctest --test-dir build-postgain --output-on-failure --timeout 180 -j 2
```

Result: **75/75 CTest targets passed**. The initial test invocation was stopped
after the new retune fixture used a frozen test clock inside a bounded sleep;
the fixture was corrected to let time advance during actual retune and the full
suite was rerun successfully. No device operations were performed for this run.
The six code/build/test file hashes were matched against the publication source.

New checks cover exact environment values, stable-window boundaries, unlock and
clock-reversal reset, original A lock-call control sequence, B's exclusion of
the pre-gain sample, gain-once behavior, real backend retune reset, and bounded
trace saturation/milestone reservation. Existing lifecycle tests also passed.
The A sequence comparison covers request/value/index for the lock/gain call;
it is not a byte-for-byte recorded hardware trace of a complete tune/capture.
An additional B-specific outer-service timeout/cancellation integration test
has not been added; those shared service paths were not changed and their
existing tests remain part of the full suite. Hardware outcome is unverified.
