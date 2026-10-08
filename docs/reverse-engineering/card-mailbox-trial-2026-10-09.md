# Direct card mailbox trial, 2026-10-09

The W3U3 internal reader completed ATR acquisition, portable px4 T=1
negotiation and a standard initial-settings APDU through direct libusb.
No official SDK or legacy kernel module ran in this process. The code was
committed as9563137 after the trial.

```sh
asicend --hardware --primary 1:102 --primary-port 1-2.1 \
  --sibling 1:114 --sibling-port 1-2.2 --probe-card
```

Latitude was running AnduinOS/kernel7.0.0-34-generic. Both USB functions
were owned together; no capture ran concurrently. These USB addresses are
historical observations. The private source snapshot SHA-256 was
`129ac3a04df3bf471c675574c9172060e0e9ae599375254646a5b23bdd95d912`.

The diagnostic returned0 in1.369seconds:

- Valid ATR:13bytes, supported19200/LRC profile.
- Upstream T=1 initialization completed.
- Initial-settings APDU9030000000 returned61bytes and SW9000.

The61-byte payload is not printed or committed because it includes card
material. This proves card communication, not PC/SC integration or recisdb
decoding. Removal/reinsertion and concurrent reception are untested.

## Trace and cleanup

Tracing started before frontend initialization. The private trace contains
660records, zero kernel drops and zero matched vendor-control completion
errors. Controller00 commands include reset80/00, activation08/09, four
consumes0c (ATR and three T=1 responses), three submissions0a and final00.
The trace has28mailbox-window reads.

No LNB-mask or GPIOEx write occurred. GPIO first/last responses areff;
the last controller05 read is00. Card cleanup wrote04=00 then00=00 before
shared GPIO restoration. tcpdump was explicitly stopped. Both USB functions
remain idle on the host for the next card/PCSC increment at this checkpoint.

Raw trace and diagnostic files are retained outside Git, with a persistent
0600backup at `/config/.tools/asicen-work/card-probe-private-20261009.tar`.

## Software validation

The new mailbox tests cover byte reversal, numeric USB encodings,8-byte
window/64-byte page boundaries, malformed/incomplete frames, ACK/short/NACK,
cancellation and changing response length. An end-to-end fake connects the
imported CardSession to the mailbox and passes a65-byte T=1 frame across
the page boundary. Focused tests including the upstream card suite pass
with libusb ON and OFF; host mailbox/satellite tests also pass.
After the trial, Latitude's full Release/libusb-ON suite passed36/36tests
in34.45seconds using the same isolated source snapshot.
