# W3U3 terrestrial startup: post-gain readiness A/B diagnostic

The original opt-in investigation build is
`6dae08a784cf78613b0013ada766c57b30b8c65a`, based on evidence commit
`a092650dee1e851150a15259d36377b56d12ff86` after candidate
`b64577e94389fe479d573eed57c745e2eb5573ef`.
This revision makes the tested B readiness behavior the default in a fix
candidate. It is not a verified hardware fix or a release authorization.
The pinned PX4 v0.2.1 source is unchanged. Trace recording remains opt-in.

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

- Unset: the same 340-ms readiness behavior as B below, without diagnostic
  event recording. In the historical `6dae08a` build only, unset selected the
  original return behavior; do not confuse the two defaults.
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

## Historical minimal E02 comparison

The original comparison used one Linux musl x86_64 diagnostic archive for
**both** A and B, with
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

Changes are confined to the investigation/fix-candidate branch. Do not merge or tag it as a
release merely because offline tests or these short trials succeed.

## Offline verification before publication

On 2026-10-13 UTC, the Latitude build executor completed:

```sh
cmake -S . -B build-postgain -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DASICEN_BUILD_TESTS=ON -DASICEN_ENABLE_LIBUSB=ON
cmake --build build-postgain -j 4
ctest --test-dir build-postgain --output-on-failure --timeout 180 -j 2
```

Historical diagnostic result (`6dae08a`): **75/75 CTest targets passed**.
The initial test invocation was stopped
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
That diagnostic did not add a B-specific outer-service timeout/cancellation
integration test. The candidate must verify those paths as described below;
the historical pass is not a test result for the changed candidate.

## Warm E02 evidence and fix-candidate boundary (2026-10-14 JST)

The [initial five pairs](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6150816682)
and [additional ten pairs](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6151079359)
used diagnostic `6dae08a`, identical hashes, warm W3U3, T27 receiver 1,
fresh daemons and 10-second captures, always A then B. A failed 2/15 times
(A3: 39 CC events; A9: 48), while B failed 0/15. All 30 runs had zero
reported TEI, sync, queue and USB errors and daemon exit 0.

A9's final CC event was detected at packet 356, beyond its first 346 emitted
packets. Detection position does not locate the physical disturbance in time;
it compares the current packet with the previous packet of the same PID.
Do not infer that all errors fit into one block or add prefix suppression.

The gain-to-first-worker-delivery interval was 192.6--245.9 ms for A and
553.2--569.2 ms for B. This supports preparing a conservative readiness
candidate, not causation, a safe minimum, or a zero failure-rate claim.
Fixed order, one channel/device and warm-only operation limit the evidence.

The candidate's default now follows B without tracing. Explicit diagnostic
`0` remains available only for controlled comparisons, and `340` enables the
same wait plus trace. Validate the normal unset path as well as the traced
path; old A/B evidence does not certify this rebuilt binary.

Before hardware handoff, verify fresh post-gain samples, observed unlock
reset, retune/close reset, outer timeout/no-lock handling and interruption
during readiness with offline tests. Then coordinate short channel/receiver
regression and a counterbalanced comparison with the owner. These are proposed
checks, not automatic physical-operation instructions or new soak gates.
Long soaks remain deferred until the end under the existing user decision.

The separate cold firmware upload IO failure is unresolved. Warm tests do
not exercise it. LNA initial values, shared PX4 code, transfer/framing behavior
and firmware loader code are unchanged by this candidate. No release, tag or
new model/platform hardware acceptance is implied.

## Fix-candidate offline verification

On 2026-10-14 JST, the Latitude executor applied the candidate patch to a clean
detached `6dae08a` worktree. Release/Ninja configure and all 267 build steps
passed; CTest passed **75/75**, and `git diff --check` passed. All four changed
code/test file hashes matched the publication inputs. This is offline evidence,
not hardware acceptance. The separate cloud C++17 readiness unit check also
passed with strict warnings, no exceptions and no RTTI.

The lifecycle target now runs the unchanged real PX4 `TunerService` against
the fake-USB ASICEN backend with its normal default configuration. It covers
successful readiness (35 outer 10-ms polls), original-budget timeout, never-lock,
stop requested between readiness polls, same-lease retune failure/recovery and
release/reacquire. Failed readiness keeps streaming unavailable. Cancellation
inside an in-flight USB operation is not newly tested here.
Default-versus-traced-B tests compare lock/gain USB request/value/index sequences
and verify the normal path records no trace. Existing explicit-A and sampled
unlock tests remain. Run default-path tests with
`ASICEN_DIAGNOSTIC_POST_GAIN_MS` unset.
