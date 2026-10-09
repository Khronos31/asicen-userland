# USB ASICEN model support for 0.1.0

Reviewer and integration: **dot (OpenAI)**. Source baseline:
`2718916cce028055290d5561ffde2df2d0153312` (2026-10-09 JST).
This work executes only offline tests and static analysis, not hardware or vendor
binaries. Hardware-unverified means implementation and protocol evidence exist;
it does not mean a receiver was tested, all silicon revisions work, or every
physical tuner is currently exposed.

## Models and operational boundaries

| Model | Runtime VID:PID | Physical capacity | Backend receiver view | Frontend |
|---|---|---:|---|---|
| PX-S3U | 0b06:0001 | 1 combined T/S | 0:T-or-S, one exclusive lease | Source-specific FC0012 / satellite |
| PX-S3U2 | 0b06:0003 | 1T+1S | 0:S, 1:T, one exclusive lease | Source-specific FC0012 / satellite |
| PX-W3U2 | 0b06:0004 | 2T+2S | Primary 0:S, 1:T only | Shared original W3U3 profile |
| PX-W3U3 | 0b06:0005 | 2T+2S | Primary 0:S, 1:T only | Existing original profile |
| PX-W3U3 V2 | 0b06:0006 | 2T+2S | Conditional primary profile; see V2 restrictions | NMI / TDA2014x / TC905xx |

The W3U3 hardware results in HARDWARE-VALIDATION.md predate these model changes.
They are not hardware validation of the new branch or of another model. The
other four products have not been tested here. Four-receiver enclosures still
reserve the sibling interface, but this backend exposes only the two operational
primary receivers, using IPC count2/mask01. The catalogue's capacity4 is physical
metadata, not a promise of four simultaneous captures. Secondary reception and
multiple simultaneous leases remain outside this implementation.

Runtime support remains conditional on the implemented bridge revision11/52,
controller address4a/type0f and version7 link path. Other detected revisions or
controller types fail before unsupported frontend/link operations. These guards
are deliberate protocol boundaries, not claims that every unit of each model
contains that revision. In particular revision16/52 has a separate ASICEN link
path which is not implemented here.

S3U/S3U2 and V2 cleanup uses the source-defined board-off sequence, not a
claim of restoring arbitrary previous electrical state. GPIOEx reads report
pin levels, which are not a snapshot of the output latches; those readings
must not be blindly written back as an exact restoration.

GPIO/LNB electrical verification is deferred until measurement with a physical
tester. This is **not an implementation prohibition**. Model-specific official
board initialization includes its real GPIO/GPIOEx sequence. Do not substitute
W3U3's GPIO meanings for S3U/S3U2. The LNB extension implements source-backed
0/15-V requests for W3U2/W3U3/V2; S3U/S3U2 reject software-enable requests
because their recovered official setters do not perform I/O. Board
initialization is not a measurement or guarantee of antenna voltage.
See [LNB operation and limitations](lnb-control.md).

## Why these are separate implementations

- W3U2: official W3U2 and W3U3 x64 BDA binaries in the original W3U3 package
  are byte-identical, SHA-256
  `a032a28b28e5d239aa32b9c41e7d9f61c810ada4b120dcc214c58be770ea5b88`.
  The Linux package also selects the W3U3 library for W3U2.
- S3U: one receiver uses hardware lane0/endpoint81 for **both** systems, with
  system-specific demod30/32 selection. It has a13-entry terrestrial table,
  its own power/startup sequence, band prewrite through demod32, and final
  demod30/0f=14(T) or3c(S). It must not route terrestrial receiver0 to local1.
- S3U2: separate local0:S/local1:T,13-entry terrestrial table, extra FC0012
  register10 pulses, dedicated power/GPIOEx sequence and one-pass tune wrapper.
  The DTV_Init pre-power GPIO08 release matters in addition to TC_PowerTunerDemod.
  Its default gain-poll state writes tuner13=0f; it is not W3U3's retry/gain loop.
- V2: NMI family chip-dependent initialization, different tuner bridge formats,
  satellite PLL and demod addresses. Its roles and RF master are derived from
  customer information, not inferred from USB enumeration order. No FC0012
  profile is applied to PID0006.

Source anchors are embedded beside the new plans and tests. The common FC0012
path also fixes three previously identified source-order defects: duplicate
initial VCO pulses, missing high-VCO delay, and rereading the retry-reset snapshot.

## V2 source and ownership boundary

Customer_Info response byte57 supplies the role in its top two bits, remapped
0,1,2,3 to0,2,1,3. Roles0/1 share RF master0; roles2/3 share master2. Streaming
remains attached to each USB function. This backend's primary pairing must be
source-validated before RF programming; an arbitrary port-sorted device must not
be treated as the master. Opaque A8:b0 association bytes are compared in memory
and never logged. The legacy identity read follows board startup, so no failing
pre-power probe is treated as proof of an unrelated device.

The isolated V2 frontend is an actual implementation with chip-family checks,
bounded reads/writes and failure handling. Generic source-driver labels alone
were not considered proof: official NM120/NM130/NM131/extended-family init
branches and the ISDB-T mode were inspected independently.

The portable reference was pinned at `knight-rider/ptx`
`ad3dc2619787a9a38ae3c5a17137f47d9631e8e1`:
- nm131.c: SHA-256 `fd35d5a07754627d8bea839c21cc041bcd389cbaf04f0be398faf6240a264adf`
- tda2014x.c: SHA-256 `283d92324eab103d6071e7a1937ac7f1a8ce2ebac759833eb21b7a936d25a3af`

## Firmware is model-selected, not just size-checked

| Runtime model selection | Verified external image SHA-256 | Start/index |
|---|---|---|
| S3U / S3U2 / W3U2 / W3U3 | b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8 | 5399 |
| W3U3 V2 | 5db39facda140bdcb3a238dad10da5aac752a1ba3aaf17e2239f8d79a0ad6a2a | 55d6 |

Both images are16384bytes and use20transfers over the same four uploaded ranges,
but they are not interchangeable. Loader IDs1738:5211/5216 do not establish the
runtime model. The CLI therefore requires an explicit model and checks the whole
image fingerprint **before USB access**. V2 loader acceptance is limited to5211,
as established by its official INF.

The Windows loader SYS shared by W3U2/W3U3/V2 has SHA-256
`edd9badb7588f59c6438e1ceaa0e0f3f33b2ce75667e541e51d75c45e2c485e5`.
Its statically decoded image differs from Linux. The new extractor checks the
input SYS fingerprint, standard AES transform, roundtrip, output fingerprint
and USB descriptor structure. It does not execute the loader or embed its key.

```sh
python3 scripts/extract-windows-loader-firmware.py /path/to/HDTV_PX_W3U3_Loader.sys /private/asicen-v2.bin
build/asicen-probe --device BUS:ADDR --model w3u3-v2 --firmware /private/asicen-v2.bin load-firmware
```

The extractor requires Python's cryptography package. The Linux loader image is
checked out at `firmware/asicen-loader.bin` so runners can put that exact file
into distribution archives. Corresponding-source archives still omit it.
Redistribution rights remain unresolved. The V2 image is not that file and is
not in the repository. Recheck actual runtime PID and physical path after upload;
successful writes alone are not proof of re-enumeration or receiver readiness.

## CLI and offline use

```sh
build/asicend --models
build/asicend-mock --model s3u --runtime-dir /tmp/asicen-s3u --instance test
```

The model catalogue does not access USB. The `asicend-mock` offline service
exercises topology and IPC only; its synthetic packets prove nothing about a
physical receiver. Hardware identifies the actual descriptor and cross-checks
optional --model. Single-function models omit the sibling --usb-path; paired
models require both.

```sh
build/asicend --model s3u2 --usb-path BUS:ADDRESS --instance s3u2
build/asicend --model w3u2 --usb-path BUS:ADDRESS --usb-path BUS:ADDRESS --instance w3u2
```

The diagnostic asicen-frontend tool retains its original W3U2/W3U3 plan surface;
use the model-dispatched daemon for S3U/S3U2/V2. It refuses other families instead
of silently applying an original W3U3 diagnostic plan.

## Verification

Offline verification on 2026-10-09 JST:

- GCC 14.2 Release builds completed with libusb enabled and disabled.
- libusb enabled: 40 of 46 CTest cases passed; disabled: 39 of 45 passed.
  The same six socket-dependent integration cases fail in this cloud sandbox
  because AF_UNIX socket creation is denied (EPERM), including on the unchanged
  baseline. These are blocked integration checks, not a complete green run.
- New model CLI, canonical IPC layouts, legacy frontend plans, V2 frontend/NMI,
  firmware identity, ownership and fake capture lifecycle checks passed.
- Independent static review checked source startup/off ordering, model dispatch,
  GPIOEx pin-versus-latch semantics, deadline-bounded cleanup and V2 routing.
- The satellite gate deadline regression test uses an injected monotonic clock:
  100 focused repetitions passed. A mutant that restarted the deadline after
  acquiring the gate failed the remaining-budget assertion as intended.
- Focused frontend/firmware sanitizer checks passed with leak detection disabled
  for this environment. No hardware or vendor binary was executed.

The dedicated branch runs Linux and macOS CI to cover the integration checks
that cannot execute here. Consult the exact branch commit's Actions results;
local blocked cases must not be reported as passes. No hardware results are
inferred from mock, wire-plan, source-differential or fixture tests.
