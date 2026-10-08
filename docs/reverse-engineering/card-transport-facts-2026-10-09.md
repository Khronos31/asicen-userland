# W3U3 card mailbox and T=1 evidence

This extends the lifecycle note using DTV_Lib.o and the privately retained
official `host-seed-key2.pcap`. No card payload, identifier, or key is included.

## Source and observed sequence

`DTV_PollingThread` uses the primary shared context. At9b0d–9bb3 it writes
controller4a/00=80, waits10ms, writes00=00,04=00,00=00. At9c0c it waits100ms,
reads04 and requires bit0. At9c76–9d09 the type0f path waits50ms, writes00=08,
04=80, waits200ms, then00=09. The intervening legacy GPIO branch is skipped
for types0f/10. `bReadBCAS_Data` with ATR mode1 follows; 9d2b–9d53 writes01=01
after that read. Source loop counters must be replaced by bounded monotonic
deadlines with checked USB status/length/ACK.

The recorded transaction agrees: after00=09, length03 grows2,5,12,13 while
04 reads81 and39 reads00. Page3a=00 precedes window reads40(length8) and48
(length5), then00=0c consumes the response. Per-byte bit reversal yields an
ATR of13 bytes, valid TS, expected parsed length13, TD protocols1/1/15,
zero historical bytes and valid TCK. This is an actual ATR, not a fabricated
PC/SC response.

`ChangeEndian`1d0–218 reverses the bits of each byte without complementing
them. `Get_BCAS_INFO`8860–8a1f constructs a T=1 I-block around
`90 32 00 00 00`, with alternating PCB00/40 and an XOR LRC. The local
libaribb25 source independently names this the card-ID command; its initial
settings command is90 30. The two must not be confused.

After ATR, the same trace has12 reconstructed complete T=1 response blocks:
lengths23,65, then ten29-byte blocks. All have matching LEN+4, valid LRC and
SW9000; PCBs alternate00/40. This includes a64-byte page boundary crossing.
An additional consume operation has no ordinary read window in this trace
and is not included in these12 responses. These are reference-driver
observations, not a successful portable-reader experiment.

## Mailbox rules

- bReadBCAS_Data8030:03 is low available length;39 bit0 is its ninth bit.
  **04 bit0 is separate** and is required by the reader. It is also polled
  periodically at917a–91ad, where clearing it invalidates card state1340.
  It must not be described as the length high bit or sufficient evidence of
  a complete response. Confirm physical-removal behavior independently.
- Reads wait for the ATR minimum13 or a T=1 header-derived total, then page
  through64-byte windows40..7f and consume with00=0c. Do not consume partial
  blocks merely because a nonzero length appears.
- bWtBCAS_Data8600–8859 writes length low02 and high bit38, selects page3a
  at each64-byte boundary, writes data, submits00=0a and waits100ms.
  The length field and payload use different encodings: bit reversal applies
  to T=1 bytes, not mailbox length/status registers.
- DTV_WriteI2CEncData7f50 sends multi-byte window writes through mode2
  staging with the window register prepended; single bytes use normal I2C.
  DTV_ReadI2CEncData7e80 uses normal I2C for multi-byte reads. The traced
  window chunks are at most8 bytes.
- bBCardUninit6bf4–6c4e writes04=00 then00=00 and waits50ms on the applicable
  primary path. Card cleanup must precede shared frontend power restoration.

## Portable integration boundaries

The px4 CardSession already supplies ATR parsing, T=1 LRC/CRC, sequencing,
chaining, IFS and WTX handling. Prefer adapting CardHardware with a bounded
mailbox buffer and retaining those tests, excluding only IT930x-specific
definitions. Reset should provide ATR bytes to that session; do not also run
the vendor thread's IFS/card-ID exchange and duplicate negotiation.

First test a standalone diagnostic under the existing dual-function owner,
with no capture running. The initialized controller must be revision11/type0f,
with output05=00. Preserve non-LNB GPIO and use existing shared power cleanup.
Card reset can invalidate card protocol state; no persisted card identity or
session is assumed. A successful ATR is only the first gate: verify standard
initial-settings APDU response and then actual recisdb decoding.

Concurrent capture/card operation needs explicit USB transaction serialization
because mode2 staging is shared, and card reset may affect controller state.
Do not infer concurrency safety from separate working standalone tests.
