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

The read protocol is the ordinary I2C mode-0 path: request `0x02`,
`wValue=(register<<8)|slave`, `wIndex=0`, vendor-IN, one status byte followed
by payload; status must be `0x01`. This is the mode used by the recovered encryption-chip read
helper and is independently observed for controller reads. Before applying a
seed, the tool snapshots controller register `0x05` and link registers
`0x10..0x1f` at slave `0x4a`. After the existing DSC start and queued host
submission, it writes each seed byte with request `0x03`, then writes
controller register `0x05 = 0xa0`. Every write checks full transfer length and
status; the new state is read back before capture proceeds.

On completion or error, DSC is stopped and queued transfers are cancelled and
drained before the saved seed bytes and controller `0x05` are restored and
read back. If DSC stop fails, the tool skips link-state restoration because
the device may still be streaming; it reports an error and fails. Failed
partial application otherwise triggers restoration. Restoration write or
readback failure is a hard error. The earlier CF/DSC/tuner setup is unchanged.
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
