# Static audit: frontend ordering, firmware gates and Windows comparison

Reviewer: **dot (OpenAI)**. Starting source: `baab83e132cc737f4d78dd3b3d5a4eff4e226d04`.
Date: 2026-10-08. No receiver operation, vendor-code execution, module loading,
card operation, release, or change to main was performed for this audit.

## Result and limits

Three source-ordering defects are corrected on this branch:

1. The initial FC0012 VCO calibration pulse train was emitted twice.
2. The high-VCO fallback omitted the final 1 ms settling delay.
3. The terrestrial retry reset reread demod `1c` where the vendor uses a saved byte.

These are proven implementation differences, **not a proven explanation or fix
for zero-byte reception**. Existing hardware results remain unchanged. The
corrected shared-demod table was already on the starting commit and was not
changed here. Its corrected hardware trial still produced zero callback bytes.

The audit covers the Linux host request builders, loader/download layout,
frontend tables and tune ordering, relevant firmware request handlers, and
selected equivalent Windows BDA paths. It is not a claim that every instruction
or all ASIC internal behavior has been reconstructed. In particular, demod TS
pin/clock definitions and electrical connectivity remain unproven.

## Evidence identity and addressing

Source artifact: [asicen-research run 37686293947](https://github.com/Khronos31/asicen-research/actions/runs/37686293947).

- Linux: `ReleaseToCustomer_64bit_130109_2/libPlexLib_W3U3.a`.
  Addresses below are member-relative `.text` addresses.
- Original `TunerControl.o` SHA-256:
  `26956331982fce11b4f1b9abbfe5439fcd46f82dca1b2a1499c75f429d83e84f`.
- Linux `loader.ko` SHA-256:
  `10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0`.
- Extracted 16 KiB `FirmBin` SHA-256:
  `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`.
  Firmware addresses here are 8051 code addresses, equal to blob offset + `2000`.
  Setup-parser references and startup at `5399` corroborate this mapping.
- Windows: original W3U3 v1.1 x64 BDA `HDTV_PX_W3U3_BDA.sys`, SHA-256
  `a032a28b28e5d239aa32b9c41e7d9f61c810ada4b120dcc214c58be770ea5b88`.
  Windows addresses are disassembly VAs with preferred image base `10000`, not RVAs.

No vendor object, firmware bytes, seeds, keys, or cryptographic tables are
redistributed by this branch. Facts were obtained through static inspection.

## Corrected frontend ordering

### One initial VCO train, not two

`TunerControl.o:Adpater_SetFreqISDBT` writes tuner registers 1..6 individually
at `1d5e..1d8f`, then performs:

`0e=80 -> 0e=00 -> delay(1 ms) -> 0e=00 -> read 0e`

Evidence: `1d91..1e51`. There is only one unconditional train before the
feedback read. Before this branch, `plan_fc0012_tune` emitted that train and
then its `Fc0012VcoCalibrate` operation emitted it again in
`run_vco_calibration`. The branch leaves ownership with the composite operation
and removes the duplicate planner operations.

### Both conditional VCO adjustments settle

If high VCO is selected and `(feedback & 3f) > 3c`, `2076..2083` clears reg6
bit3 and jumps to `1fca`. If low VCO is selected and feedback is below 2,
`1fb5..1fc7` sets that bit. Both paths share reg6 write, `0e=80`, `0e=00`,
then the delay at `2034..2039`. The high branch lacked that delay in the
userland implementation. It is now present in both branches.

### Retry reset uses one saved byte

For the terrestrial retry condition (outer tuner `0e` bit6 clear, fewer than
four attempts), `TC_SetFrequency` reads demod `1c` once at `24b7`.
It writes `saved | 30` at `24e0`, waits 10 ms, writes that saved value AND `ef`
at `2513`, waits 10 ms, and reinitializes RF before retrying.

The previous pair of generic RMW operations inserted another read before the
second write. This branch uses a single snapshot for this retry only.
`TC_PowerTunerDemod` actually does reread (`29f3`, `2a99`), so power-on remains
unchanged. Error, short-transfer, deadline and cancellation checks are retained.

## Frontend facts that did not justify further changes

- Terrestrial demod 22-pair table exactly matches `.rodata` values `200..215`
  and registers `220..235`; `InitDemod` loop `1055..109f`.
- Corrected satellite 42-pair table exactly matches values `240..269` and
  registers `280..2a9`; loop `10b8..1104`. Earlier misaligned-table trials are
  not evidence for the corrected table; the later corrected trial is recorded
  separately in `HARDWARE-VALIDATION.md`.
- `TC_Initialise(local0)` performs shared terrestrial/RF/satellite demod
  initialization. The source1 `InitRFDevice` call does no I/O (`1a50..1ab6`).
  There is no missing satellite-RF write sequence to invent for that call.
- The initial 773143 kHz tune is not reproduced by the current direct T27 path.
  Its necessity after a successful T27 retune is not established.
- Source0 selects slave30 for either local; source1 selects slave32 for either
  local. Local stream lane and demod source are separate concepts.
- Normal terrestrial demod sequence `25=00`, `23=4d`, reg1e RMW, `0f=34`,
  `01=40`, `23=4c` matches the successful-path call sites. Dynamic/read-only
  bits mean intended writes are not a full prediction of readback values.
- `TC_IsLocked` (`8d0..8ff`) reads b0 and tests its low nibble; it does not
  enable TS. Named helpers `Adapter_SetTsOutput` (`140`),
  `Adapter_StopTsOutput` (`150`), `SetTsOutput` (`180`) are stubs.
- `TC_PollingThread` -> `Fiti_LAN_Gain` performs real FC0012 gain operations,
  absent in current userland. These are not a proven lock-triggered demod
  TS-enable sequence. They were not added to acquisition defaults.
- The regular I2C mode0 writes, demod mode1 reads, direct mode0 reg1c reads,
  and FC0012 FE/C6/C7 bridge packing match their short-operation call chains.

## Firmware cross-check: setup, DSC, reset and CF

### USB byte order is confirmed, not guessed from CPU endianness

Setup parser `41d2..4224` and helper `29f8` convert USB little-endian pairs:
ValueHi -> direct variable `0d` / XRAM `c638`, ValueLo -> `0e` / `c639`;
IndexHi/Lo -> `0f/10`, LengthHi/Lo -> `11/12`.
The vendor handler requires IN direction. Its `c639` references therefore
refer to the **low** wValue byte, despite the adjacent XRAM address order.

### DSC06/07 and lane encoding are correct

Dispatch `23b4`/`23c1` passes ValueLo to start `5b54` / stop `5c49`.
The helper gate `29b8` requires wLength exactly 1. ValueLo zero selects
`8400`; nonzero selects `8800`. Start sets bit7, stop clears it.
This independently supports current start06/stop07 with `wValue=local`.
No extra high-byte lane shift or index flag is justified.

### Reset09 has a hardware-side asymmetry

Handler `27c8` selects the stream by ValueLo, temporarily disables its bit7
if necessary, strobes `8c0a` with lane selector 1/2 followed by selector OR80,
then restores the saved stream-enable state.

At `281f`, nonzero lane skips to `2835`. Only lane0 conditionally sets/clears
`9840` bit2 according to ValueHi==1. Both lanes unconditionally set `9c40`
bit2 at `2835`. Thus request09 state0 alone does not clear lane1's bit2.
The subsequent CF block write is significant. Do not simplify the host's
read/reset/restore sequence to only request09.

The physical meaning of the `8c0a` strobe is not proven. In particular this
inspection does not prove that it clears every CF register.

### CF04/05 confirms packing but not PID semantics

ValueLo bit7 selects `9800`/`9c00`; low7 selects the byte offset. The handler
requires length >1 and transfers length-1 sequential bytes. Write order is
ValueHi, IndexLo, IndexHi, matching the current builder. There is no firmware
special case for CF41/43. The fact that `1fff/1fff` is used by the official
host does not, by itself, prove a universal all-PID pass-through interpretation.

## Windows paths versus Linux stubs

### Request21 is real, but is not a proven missing TS gate

Windows helper VA `114bc..11514` sends request21, ValueLo=local,
ValueHi=start, index0, one-byte IN response. Init callers include `1c7c0`
and `1cbd3`. Corresponding Linux calls `75d4`/`7866` reach
`FUSBDTV_Cmd_TimerStartStop` at `FUSBDTV.o:d0`, which returns zero without I/O.

The firmware actually implements21 at `2159`:
- length must be 1;
- local0/1 selects state `c48a/c48b` and counter `c504/c508`;
- start=1 enables and clears the counter only if previously disabled;
- repeated start1 does not restart the counter; other start values disable;
- main loop `48e3`/`4904` increments enabled counters;
- after a counter exceeds `00070000`, helper `5b4c` sets bit0 of `8008`.

Request22 at `2137` clears the selected counter without changing enable state.
Neither handler directly sets DSC/CF enable bits. The physical meaning of
`8008` bit0 and the counter time unit are unknown. No new timer command was
added to default acquisition, and no hardware recommendation rests on its name.

### Controller and optional input-routing chip

Windows reg05 output helper VA `24b50` agrees with Linux `6440`: expected
base value20 with the initialized flags, but own controller-success flag and
local0-peer state guard local1 writes. Windows also skips the request13
encoder-start operation when that controller path is active (`1d3cb..1d3e9`).

Windows `23f1c` probes optional D2:00. A successful probe gates setup at
`1ab68`. Local1 direct routing passes input1/output2 to `24138`, writes
D2:02 as `10 | (serial==0 ? 03 : 00)`, then D2:05=47. An indirect route is
conditioned on further capability fields. Linux `65e0` has equivalent
real I2C routing with a configurable additional bit04. The existing failed
D2 probe is not permission/evidence to replay this route blindly.

Windows ordinary startup and revision11 resume also perform broader GPIO
operations than the restricted userland startup subset (VA `148b5..149ce`,
`14e05..14e13`). Some affect the sibling or power. These were documented, not
automatically replayed. They do not establish the physical missing TS gate.

### Clean is not a substitute USB reset

Linux `ASV5212.o:860` calls `FUSBDTV.o:2f0`, an immediate-return stub.
The separate ioctl103 path is also a no-op in the recovered kernel.
No clear-halt, USB reset, URB cancellation, or DSC restart should be invented
as a replacement for this call.

## Offline verification

The new wire-decoding test failed on the starting implementation and passes
with the corrections. It checks normal/boundary/low/high calibration feedback,
exact pulse sequence and delay placement, reg6 correction, feedback-read counts,
single-snapshot retry values and both 10 ms waits. It injects an error at every
transfer in a retry-then-success trace and cancellation/deadline expiry in the
retry wait. Existing tests were not edited.

Full libusb-enabled source build succeeded with GCC 14.2, CMake 4.4.4, official
libusb v1.0.28 header and the cloud system's libusb 1.0.28 runtime.
Seventeen CTest cases passed (16 existing plus the new case).
The eighteenth, mock integration, could not pass because this execution
environment rejects `socket(AF_UNIX, SOCK_STREAM, 0)` with `Operation not
permitted`, including the supported escalation attempt. This is not recorded
as a passing test or hardware failure. No CI or hardware success is claimed.

## Next evidence and single-factor experiments

First validate the source-ordering corrections in isolation, retaining the
known T27/controller/filter/DSC/queue conditions. They can be separated into
individual changes for hardware A/B; do not combine a timer, GPIO or D2
experiment with them. A source-correct sequence still needs hardware evidence.

If no sample appears, obtain a successful official-driver USB trace in an
appropriate isolated environment. No such environment was operated by this
audit. Compare cold startup through the first nonzero endpoint82 completion:

1. USB descriptors/alternate setting and exact firmware image/stages;
2. both local initialization paths, slave30/32 tables and FE bridge exchanges;
3. actual reg0e calibration counts, read replies and delay intervals;
4. GPIO, SysCtrl, controller and optional D2 probe results, with branch guards;
5. host submissions, DSC06, CF bit3 and any conditional post-lock reset;
6. presence/absence of21/22 and elapsed time, without treating them as TS enable;
7. bulk endpoint, requested/actual lengths, status, short packets and first
   data time, separate from host ring cleanup and post-read link conversion.

Lock, libusb callback bytes, raw framing and subsequent link/B25 decoding are
separate acceptance gates. The current zero-byte results do not eliminate
unverified settings in other device states.
