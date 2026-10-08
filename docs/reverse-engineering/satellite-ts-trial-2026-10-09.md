# Primary satellite capture and card decoding, 2026-10-09

The normal px4-derived daemon/client path captured BS01 satellite TS on
primary receiver0, endpoint81. An optimized build produced 30,000 packets
with valid PAT/PMT and no TEI or continuity errors. The internal card then
decoded that recording through PC/SC and recisdb; ffmpeg decoded a video frame.

These results establish reception and offline decoding. The initial capture
also exposed a TSID readiness bug: the first list read after lock was all zero,
and the client selected0000 although the received PAT identified4010. This
initial trial therefore does **not** prove requested-slot selection.

## Initial same-source comparison

Source snapshot SHA-256:
`4ce0e3f2124fef34871b1109639ac4509009d6d71f12b73c8c61a6157a85c216`.
Latitude ran AnduinOS/kernel7.0.0-34-generic with both runtime USB functions
on the host, primary1:102/port1-2.1 and sibling1:114/port1-2.2. These addresses
are historical, not persistent configuration.

The daemon claimed both functions. Each client requested30,000 packets;
receiver0/BS01_0 ran before receiver1/T27 in the same daemon. USB monitoring
started before daemon initialization. The only build difference between the
following runs was the Release configuration; device settings were unchanged.

| Build / channel | Packets | TEI | CC discontinuities | Valid/total PAT | Valid/total PMT |
| --- | ---: | ---: | ---: | ---: | ---: |
| Unoptimized / BS01 | 30,000 | 172 | 914 | 29/29 | 73/74 |
| Unoptimized / T27 | 30,000 | 231 | 1,269 | 23/23 | 56/56 |
| Release / BS01 | 30,000 | 0 | 0 | 28/28 | 84/84 |
| Release / T27 | 30,000 | 0 | 0 | 28/28 | 63/63 |

All files were5,640,000 bytes and all clients/daemons exited0. Both Release
files also had zero duplicate-CC conflicts and invalid sync packets.
The comparison supports requiring Release for hardware trials. It does not
by itself locate the loss inside the USB/device/host processing pipeline.

Release tracing recorded6606 records, no vendor-control completion errors,
DSC06/07 with local0 for BS and local1 for T27, and nonzero completions on
endpoints81 and82. Seven bulk completions ended with cancellation status-2
during shutdown. GPIO readback wasff before/after and final controller05 was00.
No LNB-mask or GPIOEx write occurred.

## Satellite offline B25 decoding

After capture daemon shutdown, the already validated card-only build and
isolated PC/SC procedure from [the terrestrial decoding trial](pcsc-recisdb-trial-2026-10-09.md)
decoded the Release BS01 file twice using recisdb1.2.4 and the inserted card.
Each output retained30,000 packets, changed scrambled packets29,268→0,
and retained zero TEI/CC errors and all28 PAT/84 PMT CRCs. Both outputs had SHA-256:

`057a973a5a10e279942a769626e45bdf748e6195aca406829d0b20b7f859e968`.

Both decoder processes exited0. ffmpeg decoded one video frame, confirmed by
its progress counter, and exited0. pcscd, asicend and tcpdump exited0.
The card trace recorded1508 records, zero kernel drops and zero vendor-control
completion errors; GPIO stayedff and controller05 ended00. No LNB/GPIOEx writes.

This is sequential recording and decoding, not simultaneous capture/card use.
Raw streams and USB/card traffic stay outside Git in the private archive
`/config/.tools/asicen-work/satellite-initial-trials-private-20261009.tar` (0600).

## Selection readiness follow-up

The implementation now polls read-only after lock, before selecting a requested
nonzero/nonffff TSID. It reads immediately and waits10ms only between unsuccessful
reads, under the existing deadline/cancellation. This is a host readiness policy,
not a claim that zero is universally forbidden by the standard or that the
vendor uses this retry. The official TSID helper performs a single read.

Final source snapshot SHA-256:
`0205ae839ee474c3f34aa49a59d50b96b9fa9ca810a0899cf179df0a89374fc5`.
With this patch and Release, the same daemon captured these in order:

| Channel | Readback / PAT TSID | Packets | TEI | CC discontinuities | Valid PAT / PMT |
| --- | --- | ---: | ---: | ---: | ---: |
| BS01_0 | 4010 / 4010 | 30,000 | 0 | 0 | 28 / 81 |
| BS01_1 | 4011 / 4011 | 30,000 | 0 | 0 | 29 / 116 |
| T27 | — / 7fe0 | 30,000 | 0 | 1 | 28 / 64 |

Every PAT/PMT section passed CRC; sync errors and duplicate-CC conflicts were
zero. The T27 continuity event occurred at packet2, PID0100 (expected8, got10).
This startup event is retained as a limitation, not removed from the counts.
The trace confirmed both selected TSIDs, matching the two received PATs;
9652 records had zero kernel drops and zero vendor-control completion errors.
All clients and daemon exited0, GPIO restoredff, controller05 ended00, and no
LNB/GPIOEx writes occurred.

The corrected BS01_0 recording was decoded twice through the same isolated
card-only/PCSC path. Scrambled packets29,295→0, with all30,000 packets retained,
zero TEI/CC errors, and28 PAT/81 PMT CRC-valid sections. Both outputs had SHA-256
`bc74219738d878d54b75e5448d9a3cb95aabf4046354ed1ab54164fe57adfa1f`.
Both decoder processes and a one-video-frame ffmpeg validation exited0.
The card trace had1318 records and zero vendor-control errors.

A final fresh daemon after card shutdown captured T27 again:30,000 packets,
zero sync/TEI/CC errors,34 valid PAT and78 valid PMT sections. GPIO restoredff,
controller05 ended00, no LNB/GPIOEx writes, and all processes exited0.
System pcscd.service remained inactive and pcscd.socket active.

Final raw evidence is private in
`/config/.tools/asicen-work/satellite-final-trials-private-20261009.tar` (0600).
Production lifecycle tests exercise actual prepare/run/stop/DrainAdapter with
USB effects replaced, including partial submit, DSC/seed failures, local0 CF
restoration, lease rejection and receiver1 reacquisition. The full libusb-ON
suite passed40/40; OFF passed37/37 before the final readiness increment. After
the final deadline adjustment, focused lifecycle/readiness tests passed2/2.
No TSAN claim is made: the cleanup/acquire ordering test is deterministic
reentrant coverage, supported by the reservation-before-state code review.

After the final clean terrestrial regression, both USB functions were returned
to the prior CentOS VM assignment. The guest enumerated both runtime0b06:0005
functions (addresses016/017), with as11usbdtv use count0 and no receive harness.
This restores ownership, not an assertion that the original RF state is known.
