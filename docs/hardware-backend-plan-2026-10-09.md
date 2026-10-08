# First hardware connection to the px4-compatible service

Starting point:5b4fba2. Objective: record the demonstrated primary terrestrial
receiver through `asicend` and the existing `asicen-ts --channel T27` workflow.
The first increment supports receiver1 only; other receivers and card commands
return unsupported. This is a staged connection, not a change to the four-lane
target or the px4-compatible client protocol.

## Prior art and scope

Adapt the already imported px4 portable control service, leases, workers,
clients and terminal protocol. Reuse ASICEN frontend plans, one-shot gain,
four-transfer capture loop, framing and device-link transform. The original
IT930x hardware backend does not implement this device. The pinned prior-art
assessment in SPEC.md and cli-adaptation.md still applies. Do not build a new
IPC protocol, invoke the diagnostic executable as a child backend, or link the
official SDK into the daemon.

Use an explicit opt-in daemon hardware selector with primary and sibling USB
addresses and physical port paths, plus the existing instance/runtime options.
Both functions must be identified as the intended enclosure and exclusively
claimed before any vendor write. The sibling may have the known loader ID;
it is reserved, not programmed or used for capture. Reject ambiguous topology,
kernel ownership, stale addresses, missing sibling, and reversed primary port
assignment. The current singleton process lock is additional coordination,
not evidence of USB ownership. Hold USB claims until worker cleanup completes.

Extract the existing asynchronous libusb queue implementation for reuse where
possible. Use one owner for primary control/capture operations; no concurrent
retune or control access during capture. Use a bounded transformed-packet
queue and the imported stream attachment identities. Queue overflow is an
explicit terminal failure, not silent drops or unbounded memory growth.

Initialize and tune via the demonstrated safe frontend plans, poll real lock,
then perform the conditional gain step. Generate an ephemeral16-byte seed in
memory without logging it. Require idle controller05, start bulk/DSC/filter
in the tested order, then prepare the link. On stop, drain callbacks, request
seed zero writes, verify controller05=00 and restore saved CF state. Preserve
the documented inability to read back the seed latch. Snapshot/restore only
readable state; GPIO cleanup masks exclude LNB bit20. A disconnect prevents
later cleanup writes to a replacement device and fails the current owner.

The backend and data-plane adapters may share one bounded session. Start is
acknowledged only when device preparation succeeds. Detach invalidates the
attachment and wakes readers before joining acquisition. No worker is detached.
Shutdown requests cancellation before draining control/data workers, then
releases leases, hardware resources and locks. Every cleanup error remains
observable through terminal/final status.

## Executable acceptance

1. Build with libusb ON and OFF; all existing26 CTests continue to pass without
   changing their expectations. New fake-hardware tests cover setup failure,
   exact start/stop order, restart, stale attachment rejection, overflow,
   cancellation, disconnect, and cleanup failures.
2. Wrong/missing/busy USB target fails before vendor writes; offline injected
   transport checks this. A second daemon fails without disturbing the first.
3. An isolated process integration test uses the actual imported IPC and clients,
   checking finite count, stdout/file, unavailable receivers, client disconnect,
   and SIGTERM. Bounded producer memory and joined threads are checked.
4. On Latitude, `asicen-ts --receiver 1 --channel T27 --packet-count 30000`
   produces exactly30000 complete TS packets, with valid PAT and PMT CRCs.
   Record every integrity defect, not just byte count. Repeat after releasing
   the first lease. This remains unverified until the actual trial runs.
5. Duration capture returns success on its deadline rather than confusing it
   with an output-write failure. Product streaming must not inherit the research
   sink's deadline race. Unread consumer/shutdown completes without dangling
   callbacks; final terminal evidence is retained.
6. SIGTERM and ordinary client stop leave controller output stopped and both
   USB functions available for VM reattachment. Trace from before handoff,
   preserve raw material privately, and stop only tracked task PIDs.

## Constraints and rollback

No LNB or GPIOEx writes, firmware upload, alternate-setting change, hub reset,
HA change/restart, release/version bump or public visibility change. No card
backend, satellite tuning, periodic gain policy or four-lane concurrency is
claimed by this increment. Do not change the px4 reference checkout or existing
tests. All capture data and random seed material stay outside Git.

Small commits allow source rollback. In a failed trial, stop/join the daemon,
verify DSC/controller cleanup, restore saved non-LNB state where readable,
then reattach USB functions to the existing VM. If device loss prevents cleanup,
report that fact rather than claiming restoration. Physical cold recovery is
unverified and cannot be replaced by an automatic hub reset.

## Decision for independent critique

Decide whether to connect the tested single terrestrial ASICEN pipeline to the
existing px4 portable service using in-process bounded capture and ownership of
both USB functions. Rejected alternatives: subprocess CLI backend releases
ownership between operations and splits shutdown; adapting IT930x transport
does not match ASICEN. Source changes are reversible; worst case is a wedged
receiver requiring physical recovery or a blocked daemon shutdown. Affected
systems are this repository, the PX-W3U3, Latitude, and its existing CentOS VM.
Hardware cold-start and multi-lane behavior remain unverified.

## Critique and required revisions before implementation

Independent critique: REVISE. Strong objections were mixed runtime/loader
ownership, start-before-attach and detach-before-stop ordering, poisoned
reusable cleanup state, unbounded callback drain, and incomplete attachment
identity/sticky terminal checking. These are implementation requirements:

- Validate the opened device objects and the ordered sibling physical paths;
  do not reuse same-VID grouping for the mixed-ID case. Claim interface0
  directly, validate expected endpoint82 on primary at alt0, and never call
  an API that can change an alternate setting. Failed second claim releases
  the first with no vendor writes. Permit only originalW3U3 runtime primary
  and an explicitly identified matching runtime/known-loader sibling.
- Start preparation must not wait for attach. Keep the pre-attach queue
  bounded. Detach sets cancellation and invalidates/wakes readers before
  join; stop after detach is idempotent. Joins occur outside queue/state locks.
- Create a fresh capture/seed diagnostic per attempt. Cleanup failure makes
  the hardware session quarantined, not silently reusable. A DSC-stop failure
  must still attempt the independently safe controller05=00 output-disable
  if the same handle remains present; do not write seed state while DSC stop
  is unconfirmed. Record all failures, and skip later USB operations once
  disconnect is observed. Never claim seed erasure or full register rollback.
- The research void/unbounded cancel-and-drain contract is insufficient for
  daemon ownership. Normal cancellation joins all callbacks before freeing
  buffers. For a bounded fatal drain timeout/event failure, retain callback
  storage and handles; terminate the hardware daemon nonzero through an
  explicit process-fatal path without running unsafe destructors/freeing
  pending buffers. No core dump, auto-restart, reset, or claim of cleanup.
  Test this escalation in a child test process with withheld callbacks and
  show it exits nonzero within the bound. A stale endpoint after fatal exit
  is preferable to unlinking another owner's endpoint; normal shutdown must
  clean up its own socket. This is bounded fatal termination, not a guarantee
  of graceful shutdown under arbitrary libusb failure.
- Check every attachment field, preserve the first terminal failure across
  detach/stop, and retain final counters until the protocol releases them.
  Use a fresh bounded queue/framer per generation; overflow is terminal.

The implementation is authorized within the user's instruction to progress
without additional physical actions or decisions. No public release decision
is being made. 最終判断はユーザーに委ねます。
