# Card and satellite continuation

Objective: extend the px4-compatible ASICEN tools from receiver1/T27 capture
to a functioning internal card/recisdb path and satellite reception, without
LNB operations or user physical intervention.

## Increments and observable acceptance

1. Recover exact revision11/type0f card transport and original W3U3 satellite
   tune/lock/TSID operations from the saved official objects. Record offsets,
   scalar protocol facts and unresolved preconditions; do not execute vendor
   code against hardware merely to infer a missing operation.
2. Add bounded transport-independent operations with fixed numerical tests.
   Invalid input, NACK, short response, deadline and cancellation must stop
   subsequent writes and retain the failure. Existing tests remain unchanged.
3. Card hardware trial: valid presence status, ATR, repeatable permitted APDU
   response under exclusive enclosure ownership; raw card/key payloads stay
   private. Then connect the px4 CardService and isolated PC/SC adapter to
   recisdb. Acceptance is successfully decoded captured TS, not just an ATR.
4. Satellite hardware trial: tune a source-backed transponder without LNB
   writes, read lock and TSIDs, select a valid slot, obtain local0/endpoint81
   TS with CRC-valid PAT/PMT. A no-lock result is evidence, not reception.
5. Regression: terrestrial finite capture still works; shutdown joins workers,
   disables stream output and restores saved non-LNB GPIO/CF state. Record
   offline TS quality separately from unimplemented daemon quality counters.

## Constraints, alternatives and recovery

Reuse the existing px4 CardService/client/PCSC source where applicable; the
IT930x UART backend cannot directly drive ASICEN's controller mailbox.
recisdb stays a separate executable. A diagnostic before daemon integration
is cheaper than diagnosing USB protocol and IPC lifecycle simultaneously.
The official static tables/calls do not prove the antenna is supplying a
usable satellite signal or that card initialization is independent of tuning.

Work only in this project and private analysis/isolated host directories.
Do not change HA configuration, restart HA, publish, release or change version.
No LNB mask20 writes, GPIOEx writes, hub resets or firmware uploads. Preserve
both-function ownership, time limits and cancellation. USB traces and card
responses may contain keys/identifiers and must not enter Git.

Before each physical trial record owners and register snapshots; no daemon or
official harness may run concurrently. Stop/drain before restoring controller,
CF and saved GPIO state, verify readbacks, release USB and return the prior VM
assignment. RF tuner state may not be fully readable; do not claim full RF
state restoration. Quarantine uncertain cleanup and report it instead of
retrying a potentially active device. Successful prior daemon trials verified
this ownership/cleanup mechanism; new card/satellite effects need separate
verification. Hardware unavailable or missing external signal is explicitly
unverified, not a reason to label the objective complete.
# Diagnostic review gates

An independent read-only review required three refinements before hardware use:

- Bound logical mailbox frames (9-bit length), 64-byte pages and observed
  8-byte I2C windows separately. Test invalid lengths, changing availability,
  page boundaries and incomplete frames without consuming them.
- Carry an absolute diagnostic deadline through each poll and transfer.
  Cleanup gets a separate finite deadline and ignores operation cancellation
  while retaining the original failure.
- After satellite diagnostics, reinitialize and capture the established
  terrestrial channel; require CRC-valid PAT/PMT before returning the enclosure
  for normal use. Register readback alone does not prove functional recovery.

The review permits implementation with these gates; it is not evidence of
card or satellite hardware success. Full original RF restoration is unverified.
