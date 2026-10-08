# Direct libusb link preparation trials

The primary PX-W3U3 USB function was detached from the isolated CentOS VM
and opened directly on Latitude (AnduinOS), bus 1 address 102, port 1-2.1.
Host usbmon was already recording before the handoff. The secondary function
was not changed. Shared demod initialization and T27 tuning with the existing
portable frontend reached terrestrial lock A9.

## Baseline and readback limitation

With controller 4a/05 set to 20 and verified, local 1, reset state 1,
four 4096-byte transfers, filter-start, and five seconds produced zero bytes.
All four cancelled transfers also reported zero actual length. Controller05
was restored to 00 and read back successfully.

The first seed diagnostic (`d34e69b`) passed its revision/type guard:
SysCtrl02 returned status/revision/tag `01 11 52`, controller09 was `1e`
(type 0f). All sixteen seed writes and the subsequent controller05=a0 write
returned status01 with their exact requested lengths. The diagnostic aborted
because the 16-byte seed readback did not match the supplied random seed.
Controller05 read back a0, then 00 after cleanup.

All three seed-window reads (before application, after application, and after
the attempted restoration) were identical. None of the sixteen returned
bytes matched the supplied seed. A separate DSC-stopped single-register
check also returned the unchanged value after writing its bitwise inverse.
Sixteen individual reads agreed with the contiguous read. This is evidence
that these reads do not verify the written seed state; it is not proof that
the seed writes have no effect.

The official `DTV_Set_EncSeedRegASV5606` at DTV_Lib.o .text 8a20–8b10 only
writes registers10..1f and checks write results. It does not read or compare
the seed. Consequently d34e69b's seed snapshot/restoration claim was too
strong: writing back this read window and observing it unchanged does not
establish restoration of the previous seed latch.

The revised experimental contract requires controller05=00 before applying
an ephemeral caller seed. It checks each write acknowledgement and verifies
controller output state, then validates the resulting transport offline.
Cleanup stops DSC, drains transfers, requests zero writes to the seed
registers, and restores/verifies controller05=00. Seed clearing itself cannot
be verified through these reads and must be reported as such. This replaces
the newly introduced seed-restoration requirement; it does not relax the
existing DSC, CF, output-state, or device-ownership checks.

Raw captures, random seeds, and traces remain private outside Git. No LNB or
GPIOEx writes are part of these trials.

## Nonzero data and the missing RF gain adjustment

With the revised diagnostic (`2f0624a`), the arbitrary caller seed produced
204,800 raw bytes in five seconds, but no PAT/PMT. Reusing the seed from the
successful official trace produced the same byte count. Both captures
contained only the one-segment/SI PID subset, so this was not accepted as a
complete TS receive result. Seed authentication was not established as the
cause. Controller output cleanup succeeded in both cases.

Comparing FC0012 writes through the official recording interval, rather than
stopping at its first bulk completion, identified four differences:
reg0d=12 versus02, reg10=06 versus00, reg12=00 versus1b, reg13=0a versus02.
The official first bulk precedes its T27 tuning; using that early point to
compare final tuner PLL state would have mixed initial773143 with T27.

`Fiti_LAN_Gain` (TunerControl.o .text13a0) begins with reg12=00, reads12/13/0d,
conditionally writes10=00, then reads10. Writing12=00 alone still yielded
204,800 bytes, and the original initialization write12=1b was restored.
Its dynamic read value is not a writable configuration snapshot.

A subsequent bounded trial followed the actual mode2 branch using live
reads, not a fixed replay of the four final values:

- reg13 & 1f was02; gain was62, from
  `(2*(reg12 & 31) + RFGainTable[(reg12 >> 5) & 7]) & 255`.
- The table is `[10,8,6,4,2,0,0,0]`; level6 is54. The gain62 condition took
  the branch at1820–18db.
- It read0d and wrote `old & ef`, wrote10=00, read10 and subtracted3 with
  byte wrapping, read0d and wrote `old | 10`, wrote the adjusted10, then13=0a.
  The shared tail is165c–16cf. The observed final writes were12/06/0a.
- All requests checked full transfer length and status01. After each trial,
  the known frontend initialization writes0d=02,10=00,12=1b,13=02 were issued
  again. They were not described as snapshots of dynamic read values.

The same arbitrary seed now produced complete, CRC-valid program tables:

| Measurement | Five-second gain trial | Count-limited repeat |
| --- | ---: | ---: |
| Raw bytes | 9,682,944 | 5,640,000 |
| Transformed bytes | 9,682,564 | 5,639,624 |
| Complete packets | 51,503 | 29,998 |
| Valid / invalid PAT | 49 / 0 | 28 / 0 |
| Valid / invalid PMT | 113 / 0 | 62 / 0 |
| TEI / invalid sync | 0 / 0 | 0 / 0 |
| Full-file CC discontinuities | 2 | 4 |
| Discarded framing bytes / pending EOF | 224 / 156 | 268 / 108 |

All first-run CC events were at packet15/44; all repeat events were at
15/18/19/21. The first run retains a183-byte incomplete PMT at EOF, separately
from invalid complete sections. Four program numbers were recovered:
1024,1025,1408,65520. B25 scrambling remains; this is device-link decoding,
not portable card/recisdb validation or a claim of lossless startup.

The five-second command returned `output-failed` at the deadline after saving
the reported data. The sink checks the same deadline independently of the
queue loop, so the result is consistent with a boundary race; its precise
output error was not instrumented. This failure must not be silently counted
as a successful command. The repeat used `--packet-count 30000 --seconds 10`,
returned0, `completed`, `limit_reached=yes`, and `close_ok=yes`. That diagnostic
count bounds raw bytes, so framing discards leave fewer than30000 TS packets.

Transformed SHA-256 values:

- Five-second: `04732e4207469530814f044db22e1d0fdf440001a9c080a9063768986e3b7108`
- Repeat: `27467be43556a4babefe7b83e70bdf2412a45c52483a2413d8a55a36da584f4e`

This establishes direct libusb reception plus the portable version7 transform
on the tested primary terrestrial receiver. The gain step is a demonstrated
missing operation in this state. It does not prove universal gain constants,
cold-start correctness, satellite reception, or daemon hardware support.

## Cleanup

Both DSC lanes were explicitly stopped; normal GPIO was restored with maskdf,
excluding LNB. GPIO readback wasff and GPIOEx readback02. The primary USB
function was reattached to the CentOS VM, which enumerated it as device011.
The tracked host tcpdump PID88875 was stopped:9414 records, zero kernel drops.
Trace payload truncation is still possible despite that drop count. Private
raw evidence was copied to the persistent tool directory, with mode0600.
