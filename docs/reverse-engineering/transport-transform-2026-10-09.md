# Version-7 transport transform and reference DES comparison

The x86-64 official archive's multi-block DES routine is not a reliable
reference primitive. Identical synthetic inputs produced different outputs;
its disassembly stores four bytes into stack slots and later loads eight
bytes from those slots. The single-block routine passes the standard DES
known-answer and inverse checks. This is independent of RF reception.

The portable implementation preserves packet bytes 0..3, XORs bytes 4..187
using state indices `[0,0,3,1,0,2,1,2]`, then applies DES decryption to sixteen
blocks beginning at byte 4 and seven blocks beginning at byte 132, using
separate keys. The seed-to-key derivation reproduces KT1/KT2, without importing
the SDK's application-key tables. Acceptance of an independently chosen seed
by the hardware remains unverified.

An offline oracle retains official `DTV_DecrypTS` but wraps
`des_crypt_ecb_Multi` with repeated calls to official `des_crypt_ecb`.
The portable packet transform matches all 32 synthetic cases. The key
derivation matches official KT1/KT2 for 64 deterministic seeds and all 128
single-bit seeds: XOR state is compared directly, and each derived DES key
is checked against the official schedule with eight synthetic blocks.
This oracle does not open USB or execute initialization against a device.

## Controlled reference capture

On 2026-10-09 JST, the existing guarded reference harness was linked into a
separate `official-trace-harness-fixed-des` executable with that one DES
substitution. The harness object, Key2 API flow, T27 selection, 20-second
read loop and GPIO guards were unchanged. This is a modified reference-library
experiment, not an unmodified official result or direct userland capture.

Both local initializations returned 1; Customer_Info and its cached VID/PID
were `0b06:0005`. Public tuning returned 1 and RF lock reached 1. The capture
produced 39,598,816 bytes (210,632 packets) in 3,298 reads.
SHA-256: `bf40fd6e291e735e272f1eb21f3432200cf0475bded8a32733cc406cb762513e`.

| Measurement | Earlier original Multi | Single-block substitution |
| --- | ---: | ---: |
| Bytes | 38,420,244 | 39,598,816 |
| Valid PAT sections | 187 | 193 |
| Invalid complete PAT sections | 0 | 0 |
| Truncated PAT sections | 3 | 0 |
| Valid PMT sections | 423 | 444 |
| Invalid complete PMT sections | 15 | 0 |
| Truncated PMT sections | 8 | 0 |
| Sync-invalid packet slots | 101 | 106 |
| TEI in sync-valid packets | 4 | 0 |
| Full-file CC discontinuities, excluding null PID | 24 | 5 |

Both files contain corruption in the first 128 packet slots. In the new file,
all sync-invalid slots are 21..126. All five full-file CC events occur by
packet 331. The validator's independently initialized suffix (from packet 128)
counts two events because its per-PID history starts at that boundary; it is
not a contradictory full-file count. One 183-byte partial PMT remains at EOF,
which is reported separately from invalid/truncated complete sections.

The improvement supports using the single-block-compatible transform. It does
not establish lossless reception or isolate every remaining startup defect.
B25 scrambling remains present in the startup portion. No explicit card API
or recisdb trial was performed by the harness; subsequent analysis below
shows that the official library also processes B-CAS automatically.

Guest usbmon and Latitude host usbmon began before the harness initialized
the device. Host tcpdump reported 16,022 records and zero kernel drops;
usbmon payload truncation still limits raw-payload reconstruction. Both
UnInit calls and DevClose returned 1. Explicit cleanup sent DSC stop 07 to
both locals and restored normal GPIO with mask df; readback was GPIO ff,
GPIOEx 02. LNB and GPIOEx writes remained blocked.

Raw TS and USB traces stay outside Git because traces contain device-link
seed material. Only the source, synthetic oracle and compact numerical
validation belong in the repository.

## Portable transform against actual USB fragments

As a separate offline check, the trace's sixteen controller seed writes were
read into memory and passed through the portable seed derivation. All 631
fully captured successful endpoint-82 completions were considered; larger
completions with missing usbmon payloads were excluded. Each fragment needed
its own eight-packet sync proof. This produced 13,110 transformed packets
(2,464,680 bytes), with 12 valid PAT and 25 valid PMT sections and no invalid
complete PAT/PMT sections. All four expected program numbers were present.

The missing fragments cause artificial continuity gaps and incomplete PSI:
three truncated sections on PID01f0 and seven on PID03f0. This sample must not
be treated as a continuous capture. The check validates portable processing
of actual device bytes using the observed seed; it still does not prove that
the device accepts an independently chosen seed or that direct libusb startup
is complete. Seed values and raw transformed fragments were not committed.

## Automatic B-CAS processing in the reference path

`DTV_DecrypMultiTS` calls `TS_Process` after the device-link transform
(`DTV_Lib.o`, relocation at `.text+0x46aa`), with card-existence setup on the
same path. The successful trace also contains controller mailbox operations.
Thus the public stream-read API is not necessarily raw B25-scrambled output.

For video PID0100, the first quarter of the captured file has 15,584 scrambled
and 28,262 clear-marked packets. The remaining three quarters have 131,660
clear-marked packets and zero scrambled packets. Conversely, PID0100 in the
portable USB-fragment sample remains scrambled throughout: portable code
only removes the device-link transform.

An offline FFmpeg check, seeking five seconds into the reference recording,
decoded another five seconds of program1024 video to the null sink:

```sh
ffmpeg -hide_banner -v error -ss 5 -i seed-single-des.ts \
  -map 0:p:1024:v:0 -t 5 -an -progress pipe:1 -nostats -f null -
```

It returned 0 with `frame=146`, `out_time=00:00:05.005000`, and zero reported
dropped frames. Initial probe warnings about incomplete H.264 parameter sets
and MPEG-2 dimensions remain in the private stderr log. This is positive
evidence of usable reference-library descrambling after startup, not a
lossless whole-file claim. It does not validate the ASICEN card backend,
PC/SC interface or recisdb integration, which remain future work.
