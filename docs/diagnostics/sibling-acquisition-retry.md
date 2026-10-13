# Sibling loader acquisition retry

## Evidence and scope

The [E02 repetition record](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6156127140)
reports C01-C10 (USB reconnect) 10/10 successful and P01-P10 (AC plus USB
reconnect) 9/10 successful on diagnostic `9e711e1`. Do not combine these
different preparations into one failure-rate estimate.

In P01, the primary upload, primary runtime acquisition and GPIO restore
succeeded. The sibling resolver immediately returned intermediate loader 63;
opening that location failed with LIBUSB_ERROR_IO. A later enumeration showed
loader 65 on the same sibling topology. Successful P trials resolved a later
loader generation after approximately 500 ms. That duration is consistent
with the existing 250 ms polling, not a proven minimum settling delay.

The observed failure is before sibling firmware transmission. It differs
from the older `control transfer failed` record, whose target and chunk remain
unknown. This change targets the demonstrated acquisition failure, not every
possible loader error or electrical cause.

## Intended change

After a successful primary upload and GPIO restore, transient sibling open
failures can return to fresh resolution of the same selected topology within
one 10-second retry-scheduling deadline. The 250 ms retry interval is retained;
the final wait uses the remaining time rounded up to whole milliseconds.
An already-started synchronous acquisition may finish after that deadline.
Cancellation and permanent errors remain
terminal. Primary/single-device handling and the pre-existing warm-path
resolution behavior are preserved.

Descriptor validation, interface claims and firmware transmission remain
outside this acquisition retry. No partially issued firmware upload, primary
upload or GPIO operation is repeated. An IO response from a firmware request
is not converted into success or retried. No device-generation guarantee is
inferred merely from a different address or repeated successful polls.

Existing opt-in loader tracing records individual acquisition attempts. Its
fixed-capacity buffer is enlarged to preserve the bounded retry sequence;
critical failure retention and the final helper result remain separate.
Intermediate recoverable errors in a successful trace are evidence that the
retry occurred, not a final daemon failure. Judge terminal status and both
runtime identities separately.

## Verification and acceptance wording

Focused tests should inject fresh acquisition outcomes and a fake clock to
exercise success, transient failure followed by recovery, deadline, cancellation
and permanent errors. Such tests establish software behavior, not physical
reproduction of P01. Full builds and existing tests must also pass on the fixed
source before distributing it for hardware tests.

The user selected a pragmatic hardware check on 2026-10-14 JST: use the fixed
build for new C10 and P10 series. Keep the two preparation types, prior runs
and new results separate. Retain the original binaries and logs. Use the same
verified firmware, harness and diagnostic environment as the prior series;
record the new source/artifact/hash and distinct run tags. Actual USB/AC
operations are performed by the user with the hardware operator.

Before each counted startup verify both selected functions are loaders. For
each run record attempts, individual open errors, firmware completion, ready,
daemon exit, release and final runtime enumeration. Keep pre-daemon preparation
failures distinct; do not exclude a timeout or crash after daemon launch.
Stop on a final failure and preserve its full trace for assessment before
further recovery. Do not add TS reception, CC comparisons, LNB/card activity
or long soaks to these firmware-only series.

If both new series complete without reproduction, use exactly the limited
conclusion **"修正後の簡易試験では再現せず"**. This is the user's chosen basis
for proceeding, not proof of zero failure probability or a guaranteed cure.
Do not replace the required representative-machine regression on a subsequently
frozen release candidate with these focused tests. CC readiness PR #13 remains
a separate fix; do not combine its evidence with firmware trial counts.
