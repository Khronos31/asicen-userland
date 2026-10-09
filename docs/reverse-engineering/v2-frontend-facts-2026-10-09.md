# PX-W3U2 and PX-W3U3 V2 static profiles

## Evidence and limits

This work uses static inspection, clean portable implementations, and synthetic
register/USB transports. No vendor program or driver was executed, no receiver
was accessed, and no voltage or reception test was performed. This document
contains no device identity bytes, card data, firmware payload or key material.

The official PLEX inputs are:

- [W3U2 1.0.3](https://plex-net.co.jp/plex/px-w3u2/PX-W3U2.Driver_Utility_Package_Ver.1.0.3.zip)
- [W3U3 1.1](https://plex-net.co.jp/plex/px-w3u3/PX-W3U3_driver_Ver-1.1.zip)
- [W3U3 V2 1.0](https://plex-net.co.jp/plex/px-w3u3v2/Driver_PX-W3U3_V2_Ver1.0.zip)

All binary offsets below are file offsets/RVAs in the V2 x64 BDA SYS unless
explicitly marked original. Its image base is `0x10000`; add that value when
reading virtual-address disassembly.

The reviewed V2 SYS is 262,400 bytes, SHA-256
`5c7174d62eef7d704f44904ac1336261135a1edfccd2776e5e1c45ce7088f0f2`.
The original W3U3 SYS is 185,728 bytes, SHA-256
`a032a28b28e5d239aa32b9c41e7d9f61c810ada4b120dcc214c58be770ea5b88`.

Across the collected official USB INF files the runtime inventory is exactly
PLEX VID `0b06`, PIDs `0001` (S3U), `0003` (S3U2), `0004` (W3U2), `0005`
(W3U3), `0006` (W3U3 V2). Loader INF files bind `1738:5211`. This is the verified
PLEX USB inventory, not a claim about every ASICEN-based product worldwide.
PCIe and non-ASICEN USB products are not added by inference.

## W3U2 is an official shared implementation

The W3U3 1.1 and V2 1.0 packages each contain W3U2 and original W3U3 x64 BDA SYS
files that are byte-for-byte equal to the original hash above. The older W3U2
1.0.3 package also contains identical W3U2/W3U3 x64 images, 184,320 bytes,
SHA-256 `466c3390ccc7a429d073932760d42c6a688a09b773c52741f71e5d5f4f3d92a2`.
The official Linux `ShellScript_Lib.sh` separately chooses the W3U3 demo and
W3U3 library makefile for either selection 4 or 5. This is direct evidence for
sharing the original frontend implementation while retaining model/PID and
runtime controller checks; it is not a physical validation of W3U2.

All three reviewed packages contain the same Windows x64 loader SYS, 59,904
bytes, SHA-256
`edd9badb7588f59c6438e1ceaa0e0f3f33b2ce75667e541e51d75c45e2c485e5`.
Identical Windows loader images do **not** establish equivalence to the older
Linux embedded firmware. The separate firmware manifest/extractor distinguishes
the decoded Windows image and its start index from the Linux image.

## V2 source selection and shared USB owner

At `0x42b4`, vendor-device IN request `0x0c`, value/index zero, reads exactly
58 bytes directly into device extension `+0x3e8c`. There is no extra status byte
to strip. Response byte 0 controls validity; byte 57 is `support_feature`.
The pair-role mapping at `0x4326..0x4374` is:

| response[57] high two bits | Pair role |
| --- | --- |
| 0 | 0 |
| 1 | 2 |
| 2 | 1 |
| 3 | 3 |

DTV init `0xcccd..0xcd10` computes API source `2*role + local_lane`.
`0x1f5f0` swaps adjacent source numbers, so internal even sources are terrestrial
and odd sources are satellite. Demod addresses are 8-bit wire addresses:
`2 * (0x10 + (internal_source & 7))`, proven by `0x17464`.

For the normal role0/role1 quartet:

| USB role/local | Internal source | System | Demod slave | RF USB owner |
| --- | --- | --- | --- | --- |
| 0/0 | 1 | S | 0x22 | role0 |
| 0/1 | 0 | T | 0x20 | role0 |
| 1/0 | 3 | S | 0x26 | role0 |
| 1/1 | 2 | T | 0x24 | role0 |

The ownership table at `0x10b31..0x10bfb` has RF owners `[0,0,2,2]` and stream
owners `[0,1,2,3]`. Selection at `0x10c06..0x10c43` populates logical context
`+0x1d38`, copied at `0x10d8a..0x10daa` to per-frontend configuration `+0x168`.
Both the write helper `0xaddc` and combined read `0x164c8` dereference that owner
and serialize at the shared master, including the complete staged-read sequence.

Windows groups sibling functions by equality of an opaque 16-byte hardware
field, obtained with I2C slave `0xa8`, register `0xb0`, mode 0. At
`0x4b75..0x4bb1` it reads one byte, waits 10 ms, then reads 16 bytes. This occurs
after revision11 GPIO startup. The revision16 path skips this old identity read.
The field is neither proven to be a USB serial nor a ContainerID. Its contents
must remain private; matching requires a unique role/master and physical
board association, not enumeration order or PID alone.

Wrapper `0x1f544` runs common RF initialization `0x1f158` only when API source
is divisible by four. That common call initializes four demods/RF devices using
the same master. The portable single-target initializer must therefore be called
for the four targets by the enclosure owner; it is not four independent power
operations.

## V2 frontend and wire format

The frontend differs from original W3U3's FC0012 and fixed satellite table.
The actual ISDB-T path programs the Newport Media family; its accepted chip IDs,
family differences, checked implementation, and fixtures are documented in
[v2-nmi-families.md](v2-nmi-families.md).

The satellite RF path matches the NXP TDA2014x register model. Its bridge at
`0x1f038/0x1f09c` writes `fe,a8,register,value`, and reads by first writing
`fe,a8,register`, then `fe,a9` with repeated-start semantics, followed by the
ASICEN no-preceding-write read request `0x19`.

NMI bridge `0x1c448/0x1c4f4` uses `fe,ce,register_high,register_low` followed by
little-endian data, and `fe,cf` before its read. Tuner and demod writes are
staged with requests `0x0d/0x0e`: mode2 commit has high value byte 0; mode3
commit has high value byte 1. The combined read then uses `0x19`, not a normal
`0x02` request with mode2. Every transfer length and I2C status is checked.

The terrestrial wrapper at `0x1f69c` snaps three special frequency ranges, floors
to whole MHz, then `0x1f334` adds 143,000 Hz. Satellite input is RF kHz and becomes
`(RF_kHz - 10,678,000) * 1000` Hz before the TDA kHz adapter.

Satellite PLL facts:

- Crystal is 27 MHz; initial IF is 1,318,000 kHz (`0x1ed0c`).
- LO divider boundaries are 1,075,000 / 1,228,000 / 1,433,000 / 1,720,000 kHz,
  selecting divider 8/7/6/5/4 (`0x1d968`).
- Register21.bit6 and register1b.bit0 select integer-divider bounds.
- Exact multiplication/division, nearest-ten rounding, and the source's
  floor-half plus floor-quarter calculation are preserved, rather than the
  GPL reference's simplified rounding (`0x1c730`, `0x1d7e0`).
- PLL writes target register3 bits6..7, register1a bit5, register1e and the
  big-endian 16-bit fraction at registers1f/20.
- POR readiness is limited to three reads; channel VCO readiness to two.
- Initialization's default tune does not run demod prepare/acquire. A requested
  retune prepares the demod before RF programming, then waits250ms before acquire.
- Gain profiles depend on satellite index (`internal_source >> 1`): base gain
  bytes are `a2,b3,b3,b7`, amplifier values `3,3,6,3`. The source first clears
  tuner06.bit6 and writes tuner07 `(old & ac) | 02`. These differences are absent
  from the GPL reference's single default profile and are implemented explicitly.

TSID readout reads big-endian pairs in demod registers `ce..dd`; selection writes
`8f/90` (`0x1f790`, `0x1f8b4`). The official selector does not read back the
selected TSID, so successful writes are not advertised as reception validation.

## Power, transport and controller scope

The revision11 startup prefix/tail and shared power on/off plans have separate
APIs. They include the vendor's shared reset lines and require enclosure
ownership. GPIO responses are pin states, not I2C status bytes.
`plan_v2_shared_power_off` matches `0x1fdbd..0x1fe1c`: setGPIO40, wait10ms,
setGPIO08, wait100ms. It does not infer GPIO20's electrical LNB role or restore
unrelated snapshot bits. Electrical/voltage validation remains outstanding.

Frontend initialization returns flags `0xe9` at `0x1f5d7`. DTV init stores bit1=0
and bit3=1 at `0xd107..0xd13f`. For controller types03/0f/10, output helper
`0x1603c` combines bit10 from bit1, bit20 from bit3, and bit80 from established
link state. Thus the existing prepared `controller05=a0` mode is source-backed
for the guarded revision11/type0f path.

DTV init selects transport version7 for controller03/0f/10 at
`0xd351..0xd380`, and version9 for12/13/14. Those latter types skip the legacy05
write. The source does not justify accepting those types with the v7 backend.

The following V2 routines are instruction-equivalent to the original x64 driver
after normalizing relative calls/RIP addresses:

| Function | V2 RVA | Original RVA |
| --- | --- | --- |
| CF 0x45-byte block/reset bit2 | 0xa6dc | 0x9d68 |
| CF40 low-bit enable | 0xace8 | 0xa3d8 |
| PID boundary | 0xb45c | 0xaab0 |
| DSC start | 0x1774 | 0x1578 |
| DSC stop | 0x17d4 | 0x15d8 |
| Channel reset | 0x19d0 | 0x17d4 |

V2 initialization enables CF40 bits0/1; stream startup also sets bit3. The
existing backend sequence preserves those settings and uses the same two local
USB stream lanes. Its supported subset remains subject to actual revision,
controller and complete-source ownership validation.

Both original and V2 Windows images also contain a revision16 path. V2 detects
`16 52` at `0xcf19..0xcf65`, writes SysCtrl0a per lane, and uses a separate
vendor request0x16 link-buffer upload at `0x120dc`. That is not permission to
remove the revision11 guard without implementing and testing the additional
protocol. No physical evidence in this work establishes which revision a
particular V2 enclosure contains.

The card mailbox retains length02/38, available03/39, status04, page3a and
window40..7f. V2 write routine `0x14134` changes post-submit waits to150ms below
10bytes,140ms above70bytes, otherwise100ms. Read `0x14384` removes old initial
50ms sleeps and changes a poll limit; portable monotonic deadlines take priority
over copying unbounded vendor loops.

## Implementation provenance and verification

The GPL algorithm references are pinned to knight-rider/ptx commit
[`ad3dc2619787a9a38ae3c5a17137f47d9631e8e1`](https://github.com/knight-rider/ptx/commit/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1):

- [nm131.c](https://github.com/knight-rider/ptx/blob/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1/drivers/media/tuners/nm131.c),
  SHA256 `fd35d5a07754627d8bea839c21cc041bcd389cbaf04f0be398faf6240a264adf`
- [tda2014x.c](https://github.com/knight-rider/ptx/blob/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1/drivers/media/tuners/tda2014x.c),
  SHA256 `283d92324eab103d6071e7a1937ac7f1a8ce2ebac759833eb21b7a936d25a3af`

Both declare GPL and credit Budi Rachmanto/AreMa. The portable adaptations use
GPL-2.0-only, consistent with the combined product license. Official register
facts correct source-specific differences; no proprietary executable is linked.

`v2_frontend_tests` covers source roles, all eight wire demod addresses,
frequency remaps, eight literal PLL fixtures including alternate fractional
mode and the 3/4 rounding boundary, exact staging/no-wait-read transfers,
per-source gains, delayed bounded calibration success, lock predicates, TSIDs,
all four NMI families through the real wire adapter, and every transfer failure
in a complete TDA tune. `v2_nmi_tests` separately exercises family-specific RF
and digital fixtures and failure boundaries. Strict `-Wall -Wextra -Werror`
builds and AddressSanitizer/UndefinedBehaviorSanitizer runs passed; leak sanitizer
was disabled because the execution environment cannot run its ptrace-dependent
exit check. These are software checks, not RF/TS/card or electrical acceptance.
