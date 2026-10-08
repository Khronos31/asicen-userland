# Direct link-seed capture diagnostic

This is an opt-in experiment in `asicen-frontend`; it does not enable the
product daemon or run automatically. It requires an owner-private caller seed
file containing exactly 16 bytes, owned by the invoking user and mode `0600`.
Seed bytes and saved register contents are never printed. The raw capture is
kept separate from offline packet transformation.

Example after creating the private seed through the caller's chosen secure
process:

```text
asicen-frontend --device BUS:ADDRESS --port BUS-PORT --reset-state 1 \
  --queue-depth 4 --filter-start --seconds 5 --output new-raw.bin \
  --link-seed-file /absolute/private/link-seed capture
asicen-transform --seed-file /absolute/private/link-seed \
  --input new-raw.bin --output decoded.ts
```

The frontend command is restricted to local 1, queue depth 4, filter-start,
reset state 1, and at most 20 seconds. It checks runtime VID/PID through the
existing explicit-target guard, then reads the system-control revision/tag
and controller type. The link path requires revision/tag `11 52` and
`(I2C[0x4a:0x09] & 0x3e) >> 1 == 0x0f`; those are the recovered revision-11,
type-0x0f conditions for selecting the v7 transform. The v7 packet transformer
is a local implementation; no vendor library or authorization key table is
linked.

Controller reads use the ordinary I2C mode-0 path: request `0x02`,
`wValue=(register<<8)|slave`, `wIndex=0`, vendor-IN, one status byte followed
by payload; status must be `0x01`. The recovered multi-byte seed-read helper
also routes through this mode, but device measurements showed the bytes read
from `0x10..0x1f` were identical before and after writes. That read window does
not reveal the seed latch, so the tool never calls those bytes a seed snapshot
or claims to restore a prior seed.

Before applying anything, the diagnostic reads controller register `0x05` at
slave `0x4a` and requires exactly `0x00` (output idle). After the existing DSC
start and queued host submission, it writes each caller seed byte to
`0x10..0x1f` with request `0x03`, then writes controller register `0x05 = 0xa0`.
Each write checks full transfer length and status; controller `0x05` is read
back as `0xa0`. Seed-register write acceptance is only a USB/I2C ACK, not
readback proof.

On completion or error, DSC is stopped and queued transfers are cancelled and
drained before the diagnostic sends zero writes to `0x10..0x1f`, then writes
controller `0x05 = 0x00` and verifies that controller value. Zero writes are
cleanup requests only: their effect on the write-only seed latch is
unverifiable. The tool reports `seed_state_unverifiable=yes` and fails if a
cleanup ACK or controller readback fails. If DSC stop fails, it skips cleanup
while the device may still be streaming and reports failure. The earlier
CF/DSC/tuner setup is unchanged.
For link-seed capture the raw output path must be new; exclusive creation
prevents truncating the caller's seed if output and seed paths alias.

`asicen-transform` reads the raw file in bounded chunks, waits for the
existing eight-sync TS framer, and transforms complete packets only. Its
summary reports captured bytes, output packet bytes, discarded framing bytes,
and bytes still pending at EOF. Startup discards and trailing pending data are
therefore visible; output is not presented as a lossless conversion.

No hardware run is part of the implementation checks. Synthetic tests cover
the numeric request fields, apply/readback/restore ordering, partial write,
readback and cleanup failures, stop-before-drain constraints, fragmented
framing, and packet transform composition.
