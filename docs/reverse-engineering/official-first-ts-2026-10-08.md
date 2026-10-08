# First TS capture through the official Linux SDK

## Result and scope

On 2026-10-08, the original PX-W3U3 produced 38,420,244 bytes during a
20-second local1/T27 capture through the official Linux library and kernel
driver. ffprobe recognized four programs (1024, 1025, 1408, 65520), their
PMTs, and video/audio elementary PIDs. This establishes a working reference
receive path. It is not yet an implementation of that path in our libusb
userland driver, nor a B-CAS/recisdb descrambling test.

The raw output contains a damaged startup region. All sync failures are
within its first 128 packets; the remaining 204,235 packets have correct
188-byte sync and zero TEI. Preserve the original and report the startup
region separately rather than silently treating the complete file as clean.

Independent PSI/continuity validation of the suffix after128packets found
187 complete PAT sections with valid MPEG-2 CRC, plus3 truncated/malformed
PAT assemblies. PMTs had423 valid and15 invalid complete sections, plus
8 truncated assemblies and one incomplete fragment at EOF. There were
20 continuity-counter jumps, no exact duplicate packets and no same-CC
conflicts;18 explicit discontinuity indicators were treated as resets.
Thus this is genuine TS acquisition with remaining integrity defects,
not a lossless recording. Sync/TEI alone would have missed those defects.
The corruption is not exclusively a startup issue. Its cause remains
unresolved; several malformed section starts fall at the same offset
modulo64packets, making SDK read/transform boundary handling a candidate
for the next controlled diagnostic, not an established cause.

## Missing public API preparation

The tested order is DevCreate, AssignDevExt_0, TF_bGetCusInfo, Init for
local0/local1, TF_DTV_GenEncSeed for local1, public T27 frequency selection,
public lock polling, then TF_DTV_StreamDataRead. Customer_Info completed
with USB status0 and actual length58. Both Init calls returned1.

Earlier public tuning calls returned1 while control+0x30d60 remained1;
that branch does not tune. A successful matched GenEncSeed path clears
this readiness gate. Its inputs can be derived from the distributed SDK:
the initialized row0 candidate marker is1, identify mode is0, and the
function compares APEncSeed against an exported table row XOR its fixed
16-byte mask. The harness uses this public function, without patching
internal flags. PCKey is a fresh host-generated 16-byte value. Neither
input buffer is logged.

There are two materially different output paths:

| Public API input selection | State at control+0x30da1 | Read path | Observed output |
| --- | --- | --- | --- |
| Key1 row0 | 0 | DTV_EncAES_MultiTS | 39,714,436 bytes; 188-byte framing but unusable PAT payload |
| Key2 row0 | 1 | DTV_DecrypMultiTS | 38,420,244 bytes; PAT/PMT and programs recognized |

The read API already calls the selected transform internally. Do not
blindly apply TF_DTV_DecrypMultiTS again to its output. Key1 adds an
application-facing transform; valid sync alone did not establish usable TS.

Static anchors in DTV_Lib.o .text: Init candidate markers around0x7870;
GenEncSeed0x8b20, controller early-success guard0x8c5f..0x8ca8,
Key1 selector0x8d77, common readiness clear0x8e0e, Key2 selector0x8f5c;
StreamDataRead0x5800, alternate AES call sites0x5f5a/0x5fae.
Transform.o TF_DTV_GenEncSeed is0x250..0x302; TF_DTV_StreamDataRead is0x120.
The public table inputs select an existing SDK path, not a reproduced
original application: the inspected package contains no DemoAP caller.

## Reproduction and safety boundaries

Use [the harness](../hardware-traces/official-trace-harness.cpp) and the
existing cross-build/guest-link commands in HARDWARE-VALIDATION.md.
The new bounded invocation is:

```sh
sudo timeout -s KILL 45s ./official-trace-harness /dev/as11usbdtv0 ff \
  --seed-key2-capture /absolute/new-output.ts
```

This is specific to the inspected x86_64 SDK layout. It validates the
control pointer/back-reference, local index, initialization count,
controller/identify state, candidate marker, readiness transition, and
Key2 selector. The output must not already exist. Reads request12032
bytes, with a20-second capture deadline and4096-call limit; the external
watchdog bounds blocking vendor calls. After DevClose, the operator must
stop both DSC lanes and restore saved non-LNB GPIO bits with maskDF.
The watchdog itself does not perform restoration.

This run used CentOS6.3 kernel2.6.32-279.el6.x86_64 in the isolated guest
on the AnduinOS Latitude. It did not require changing the host kernel.
Normal GPIO writes excluded mask bit20 throughout, and no GPIOEx write
was sent. UnInit for both lanes and DevClose returned1; subsequent DSC
stops and GPIO restoration read back GPIOFF/GPIOEx02. This verifies the
digital restoration, not an electrical LNB voltage measurement.

USB tracing started before this harness's DevCreate/Init, with the
driver/firmware already loaded. This is not a fresh power-on trace.
Host usbmon recorded15,872 events, with zero kernel-reported dropped
events. Endpoint81 had3240 positive status0 completions; endpoint82
had2467. One endpoint81 cancellation had nonzero actual length.
The usbmon payload snapshot is truncated even though actual_length
metadata reports larger transfers; the pcap is not a complete raw TS copy.

## Evidence retention

- Library: official W3U3 x86_64 archive from the
  [research artifact](https://github.com/Khronos31/asicen-research/actions/runs/37686293947).
  libPlexLib_W3U3.a SHA256:
  `e1d68db2e09c584912a60363d89e56271ff3ad3b1bd0a319a7076fe914dad2c0`.
- Usable-path recording SHA256 (file checksum):
  `5c4228bd0d2efc88c14ada73cea63721e90e11d9b3d2f6407f9b004940d3c5c8`.
- Application-transformed recording SHA256 (file checksum):
  `eec5310790941f468c45a8daef0e4e03bdfb631dc312726f40479d2875c2a549`.
- Full traces, TS and probe results remain under
  `/config/.tools/asicen-work/official-trace-20261008/`, with guest originals
  in `/tmp/asicen-driver-check-20261008/` and host pcaps in
  `/var/tmp/centos63/asicen-trace/`.
- [Key2 harness diagnostics](../hardware-traces/2026-10-08-seed-key2.stderr.txt)
  contain return/state values, not seed/key buffers.
- [Validation summary](../hardware-traces/2026-10-08-seed-key2.validation.json)
  retains complete-file counts, startup/suffix packet statistics and PSI
  assembly diagnostics. Reproduce with
  `python3 docs/hardware-traces/validate_capture_ts.py /path/to/seed-key2.ts`;
  `--self-test` checks the CRC vector, synthetic section reconstruction
  and continuity handling. Null PID is excluded from CC checks. The
  validator is a bounded diagnostic, not a full MPEG-TS conformance suite;
  its exit0 means analysis completed, not error-free input.

Raw seed traces include device-link key programming in both USB setup
fields and payload. They are retained locally, not committed. Recorded
broadcast TS is also not committed. The repository remains private.

The next implementation work is to reproduce the necessary device-output
preparation and link transform in userland, then test B-CAS/recisdb
separately. This experiment does not isolate which individual write in
GenEncSeed is necessary, and does not prove a controller05=a0-only fix.
