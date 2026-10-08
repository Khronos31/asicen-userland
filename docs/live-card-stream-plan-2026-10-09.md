# Live capture and card service increment

Objective: let the px4-derived ASICEN daemon serve one primary receiver and
the internal card simultaneously, so `asicen-ts` output can be decoded by
recisdb while it is recorded. Baseline is be6b1ca: primary satellite/terrestrial
capture and separate card-only offline decoding are proven on hardware.

## Acceptance

1. Existing tests retain their expectations; libusb-ON and OFF builds/tests
   pass serially. New tests exercise production USB/callback lifecycle with
   concurrent card/control and bulk completions, cancellation, cleanup failure
   and bounded shutdown, rather than only testing a separate mock service.
2. An isolated combined daemon, private pcscd and finite `asicen-ts`→recisdb
   stream produce decoded TS during capture for T27 and BS01_0. Require valid
   PAT/PMT CRCs, no remaining scrambled packets, and a decoded video frame.
   Report TEI/continuity counts and startup effects without deleting them.
3. Trace timing establishes card APDU traffic overlapping active DSC/bulk
   reception. Merely decoding an already recorded file does not meet live use.
4. SIGTERM/client disconnect during live use joins card and capture work,
   drains callbacks before releasing memory, preserves original failures,
   restores/verifies controller/CF/non-LNB GPIO, and releases both USB claims.
5. After the new trials, finite T27 capture again produces valid PAT/PMT;
   all test processes stop and the prior USB VM assignment is restored.

## Prior art and constraints

Adapt the already imported px4-userland CardService, TunerService, control
server/client and PC/SC IFD. Their IPC lifecycle is already validated here.
The existing W3U3 mailbox backend must replace the upstream IT930x UART;
that upstream transport cannot be adopted for ASICEN. No new external library
is needed. The cheaper separate card-only process already supports offline
decoding but cannot safely claim the same enclosure during acquisition.

One active primary frontend remains the scope. Secondary USB receivers,
other terrestrial channels, LNB power and a release/publication are separate
increments. No HA configuration, secrets, firmware upload, GPIOEx write, hub
reset, physical action or system pcscd configuration change. Raw card/USB/TS
payloads remain private outside Git. Use Release for hardware acquisition.

## Hazards to resolve before implementation

- Synchronous libusb control can deliver bulk callbacks on another thread.
  An event/callback ownership mechanism must cover every callback entry and
  buffer/queue transition, including cancellation and failure cleanup.
- Card and tuner operation deadlines currently share backend fields. Separate
  contexts must prevent one operation extending, cancelling or clearing another.
- Card initialization must not retain a terrestrial frontend lease that blocks
  satellite, nor repeat shared startup during active capture.
- Controller card registers and seed/output registers share one bridge.
  Preserve transaction ordering. The control gate may cover bounded card
  sleeps, but bulk event handling/resubmission never takes that gate, so those
  sleeps cannot lock out bulk processing. Hardware independence remains an
  experimental gate.
- Shutdown must complete card cleanup before shared power/GPIO restoration.
- Shared cleanup failure must be published atomically and wake an active
  capture, causing terminal failure and bounded drain. Reject later normal
  operations; permit only independent stop/drain, card cleanup and non-LNB
  GPIO restoration. Preserve the original failure across those attempts.

## Independent review

Verdict: REVISE before implementation. A control gate and callback mutex alone
do not synchronize the existing plain `cleanup_failed_` flag or make a running
capture observe a card-disconnect cleanup failure. Accepted: add atomic failure
publication, a wake/interrupt path, sticky quarantine and genuine concurrent
failure tests covering startup and steady-state reception. GPIO restoration
remains deferred until both service workers finish. ThreadSanitizer is a
verification target; if unavailable its limitation must be stated separately.

Implementation review required two further corrections before hardware:
cleanup exemptions must nest safely so failed capture cleanup cannot suppress
independent GPIO restoration; satellite deadlines must include time spent
waiting for the control gate. Both need production-path fault-injection tests.

### Pre-existing verification issue isolated

The unchanged card-only service test's `last_operation_timeout == 15000U`
assertion fails intermittently. A fresh build from unmodified be6b1ca reproduced
the same assertion4 times in20 independent executions. Evidence is retained at
`/config/.tools/asicen-work/live-baseline-card-test-results.json`.
This separates the existing test issue from the concurrency increment; it is
not a full-suite pass. Keep the old test unchanged, report every full-suite
result, and gate this increment on the other tests plus the new production
concurrency/deadline/cleanup tests and TSAN. Repairing the timing-sensitive test
remains explicitly outstanding rather than silently weakening its expectation.

Rollback: stop all isolated clients/pcscd/daemon, bounded drain and safe hardware
cleanup, then return both functions to the prior CentOS VM assignment after a
terrestrial functional recovery check. Restore software by using the retained
be6b1ca build; do not overwrite or discard unrelated work. Uncertain cleanup
quarantines the device rather than retrying it automatically.

## Hardware pipeline

Use `asicen-ts ... --packet-count 200000 --output -` piped through `tee` to
private raw storage and `recisdb decode --input - --no-strip PRIVATE/decoded.ts`.
The local recisdb source explicitly accepts stdin (`commands/utils.rs`,
`get_file_src`); a named FIFO path is rejected as a non-regular file and is not
the selected interface. Each raw file should contain37,600,000 bytes. Keep
pcscd/IFD configuration and sockets in a private mount namespace, as in the
proven offline trial. Compare TS metadata/counts, not raw/key-bearing payloads
in console output. Summarize successful card submit timestamps within DSC
start/stop windows containing nonzero bulk completions.
