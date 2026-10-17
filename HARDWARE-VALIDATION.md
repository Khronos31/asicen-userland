# PX-W3U3 first hardware validation

## Scope

Validate the research skeleton against the attached PX-W3U3, starting from
research commit `7f4ff0e77e5570a9ac810f5742933c7135fdbc53`.
The local userland checkout retains the research history. Its intended origin
is `Khronos31/asicen-userland`. The GitHub repository was created private on
2026-10-08; public visibility is deferred to the 0.1.0 release per user
instruction. No release has been made.

## Superseding DSC request mapping correction

Static caller tracing on 2026-10-08 corrected the request mapping: DSC start is
vendor-IN request `0x06`; DSC stop is request `0x07`. Earlier capture attempts
in this report used a reversed mapping: their API-labeled start sent `0x07`
(actual stop), and their API-labeled stop sent `0x06` (actual start). Therefore
all pre-correction zero-byte capture results below are **not valid evidence
against capture with DSC actually started**. Their `01` replies only establish
that the reversed control requests completed. Re-run conclusions require the
corrected sequence and restoration checks. In queued captures, terminal
`cancel_and_drain()` preserves callback ownership but discards queued completion
payloads: the current callback path sees `actual_length`, while the drain clears
ready completions without writing or counting their bytes. A zero-byte output
therefore does not establish that no bytes arrived during terminal drain.

## Acceptance criteria and increments

1. Configure/build with libusb enabled; all existing CTest tests pass.
2. Save USB enumeration, topology, and passive descriptors; compare both ASICEN
   functions with the research loader/runtime profiles.
3. If runtime functions are present, verify endpoints `0x81`/`0x82`, then run
   `high-speed` and `customer-info` on each function.
4. If only loader functions are present, establish firmware provenance and
   validate the download protocol before sending firmware. Runtime and receiver
   verification remain unverified until this gate is passed.

## Constraints

Target only the attached ASICEN functions, identified by VID/PID and port path.
Do not change HA configuration, restart HA/add-ons, detach kernel drivers,
reset USB hubs, enable LNB voltage, access the card, or tune receivers in the
initial increment. Do not change existing tests. Keep vendor binaries and
firmware untracked. Preserve pre-existing changes in `/config`.

## Prior art and implementation gate

Adopt the supplied research skeleton for initial validation; it already provides
offline tests and a direct libusb passive/read-only probe. Firmware transfer
planning exists, but the probe has no firmware download command. Do not invent
loader requests from runtime command numbers. No production architecture is
being committed in this increment.

Cheaper alternative: use the existing probe before implementing new hardware
paths. Hidden premise: runtime enumeration is not established; the first scan
shows two `1738:5211` loader functions. Rollback: passive probes release their
handles; source changes remain isolated in this checkout. Firmware rollback
must be established separately before downloading.

## Initial observations

- Environment: HAOS kernel `6.18.52-haos`, HA Core `2026.9.4`, SCS container.
- Two loader functions: bus 1 addresses 25/26, ports `1-2.1`/`1-2.2`.
- Both negotiate 480 Mbps; interface 0 has bulk OUT `0x01`, max packet 512.
- Neither has bulk IN endpoints while in loader state; no kernel driver bound.
- `/config` contains pre-existing untracked work; it is left intact.

## Results

- Build: PASS, Clang 19.1.7, libusb 1.0.28, libusb transport enabled.
- Offline verification: PASS, all 8 existing CTest tests, including isolated
  mock daemon/client integration.
- Direct probe: PASS, `list` and `describe` on `1:25` and `1:26` match `lsusb`.
- Runtime descriptors and read-only vendor queries: PASS for `1-2.1` only;
  the sibling function is unavailable after the first function boots.
- Raw passive USB observations are kept locally in `evidence/raw/` (untracked).

Firmware evidence source: successful research workflow run `37686293947`,
artifact `asicen-driver-research`, created 2026-10-07 21:00:34 UTC. This is
retrieved for inspection; downloading the artifact does not execute its binaries.

## Firmware provenance and protocol check

- Vendor archive SHA-256:
  `11a84eaef0157ac08c0b4128aa622a625e8914a28c59ef06e76378ee6094c5de`.
- Original `loader.ko` SHA-256:
  `10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0`.
- Extracted `FirmBin` SHA-256:
  `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`.
- ELF symbols confirm `FirmBin` is 16384 bytes and `FirmwareStartAddr=0x5399`.
- Independently inspected `fusb_downloadFirmware` and `fusb_writeFirmToDev`:
  four offset/length pairs are `0000/0c00`, `2000/0400`, `2800/1000`,
  `3800/0800`. Setup type is `0x40`; offset is `wValue`, start address is
  `wIndex`; chunk size is at most 512 bytes. Only the last chunk uses `0xac`;
  all preceding chunks use `0xab`. This matches the existing 20-transfer plan.
- No vendor module or userspace binary is executed or loaded into the kernel.
- Download is bounded to the selected loader function. A partial transfer is
  not retried automatically. Return to the cold loader state may require a
  physical power cycle; that recovery procedure has not been exercised here.

## First firmware download and runtime probes

On 2026-10-08 around 11:21 JST, explicit download to bus 1 address 25
(`1-2.1`) completed all 20 transfers with exactly 512 returned bytes each.
The function re-enumerated as `0b06:0005`, bus 1 address 29, same port.

Runtime descriptor:

- Interface 0 alternate 0: bulk IN `0x81` and `0x82`, both max packet 512.
- Interface 1 alternate 0: isochronous IN `0x83`, max packet 0, interval 1.
- Configuration 1, bus-powered, reported max power 500 mA.
- `high-speed`: one-byte response, value 1.
- `customer-info`: 58-byte response, manufacturer ISDB, product HDTV PX-W3U3,
  HID HDTV IR2HID, support feature `0x0c`.

Raw customer information (no random-key/card request was issued):

```text
01ae360b06000549534442000000000000484454562050582d57335533000000004844545620495232484944000000000000000000000000000c
```

VID bytes are `0b 06` and PID bytes are `00 05`: the existing probe's
little-endian formatting displays `060b/0500`. The parser preserves bytes;
the CLI formatting was corrected to big-endian decoding and verified on hardware
as `vid=0x0b06 pid=0x0005`. The command now includes the raw response.

### Sibling impact (unresolved)

Kernel USB events after first download:

1. 11:21:18: `1-2.1` address 25 disconnected.
2. 11:21:19: `1-2.2` address 26 disconnected and began re-enumeration.
3. 11:21:20: `1-2.2` appeared as loader address 27.
4. 11:21:20: `1-2.1` appeared as runtime address 29.
5. 11:21:20: sibling loader address 27 disconnected.

Only the first runtime function remains visible. No download was attempted to
the stale address 26. Independent initialization of the two USB functions is
not established; the firmware boot has an observed enclosure-level effect.
Cause (GPIO/power/reset or another mechanism) is not yet determined. No GPIO,
frontend, LNB, or card command has been sent by the new code.

Four-receiver availability, second function boot, tuning, MPEG-TS reception,
card operation, and recovery by physical power cycle remain UNVERIFIED.

## Code validation and next increment

Added explicit firmware loading with file-size, VID/PID, descriptor, and kernel
driver checks. Download failures stop without retry or hub reset. Firmware file
reads are bounded to 16385 bytes. Existing tests were left unchanged; one new
file-validation test covers exact, short, oversized, and missing firmware.
All 9 CTest tests pass after these changes. Corrected customer-info formatting
and the unchanged high-speed query were then exercised successfully on `1:29`.

Next increment: determine the original driver's enclosure initialization that
restores the sibling function, with concrete GPIO/power evidence before issuing
writes. Do not regard a single runtime function or mock TS tests as proof of
four-receiver operation.

Red-team gate was not triggered for this local diagnostic increment and private
development push: no production configuration, public release, version bump,
or architecture commitment was made. Source rollback is the research base plus
this isolated commit; hardware recovery by power cycle remains unverified.

## GitHub CI limitation

Initial private push commit `7cbc15d` triggered workflow run `37717743836`.
The build job failed before any step ran: GitHub reported that recent account
payments failed or the spending limit needs to be increased. There is no build
log because the runner job did not start. Remote build/test verification remains
UNVERIFIED; the local libusb-enabled build and 9/9 tests passed. Resolving this
requires the repository owner's GitHub billing/settings action.

## Second bring-up increment, 2026-10-08

User confirmed terrestrial and satellite antennas and B-CAS are connected.
The primary remains runtime at `1-2.1` (bus 1 address 29); sibling is absent.
Additional read-only control observations before any new GPIO write:

| Request | Setup (value/index/length) | Returned length | Raw response |
| --- | --- | --- | --- |
| GPIOEx get `0x11` | `0000/0000/1` | 1 | `02` |
| SysCtrl read `0x17` | `0002/0000/3` | 3 | `01 11 52` |
| I2C read `0x02`, T demod reg 1c | `1c30/0000/2` | 2 | `00 11` |
| I2C read `0x02`, S demod reg 00 | `0032/0000/2` | 2 | `00 11` |

The I2C status bytes are zero; payload bytes from those failed reads are not
interpreted as register values. SysCtrl returned leading byte 1 and data
`11 52`; the original helper discards the leading byte and checks only its
ioctl result. The `11`/`16` value selects a chip revision branch, rather than
indicating initialization state. DTV_Start's shortcut checks `16 52`, so the
observed data does not meet it. No frontend readiness is established by this scan.

### Sibling restoration: observed result

Targeted only bus 1 address 29, verified port `1-2.1`, with vendor IN request
`c0:08`, value `4040` (value 40, mask 40), index 0, length 1, timeout 1000 ms.
The returned length was 1 and raw byte was `ff`; this is not interpreted as an
I2C-style success-status byte. The historical GPIO wrapper does not validate
its payload. No other GPIO bit, including the LNB mask 20, was addressed.

At 12:07:21–22 JST, sibling `1-2.2` re-enumerated as loader `1738:5211`, address
30, while the primary remained runtime at address 29. Its descriptors matched
the loader gate. Firmware download to fresh address 30 returned exactly 512
bytes for each of the 20 planned transfers. At 12:08:18 it re-enumerated as
runtime `0b06:0005`, address 31; the primary remained runtime at address 29.

Both runtime functions expose interface 0 endpoints `81`/`82`, report
`high_speed=1`, and return customer VID/PID `0b06/0005`. First acceptance
increment (two-function runtime enumeration and read-only probes) is PASS.
The physical circuitry/polarity behind mask 40 is not identified by this test;
only its observed sibling-enabling effect on this enclosure is established.
No frontend lock, MPEG-TS, or B-CAS operation has been validated yet.

### Demod read mode correction

Original-driver DemodRegRead selects I2C mode 1. Repeating the read-only
requests with index `0001` returned the following (all lengths exactly 2):

| Port/address | Slave/register | Raw response | Interpretation |
| --- | --- | --- | --- |
| 1-2.1 / 29 | 30/1c, 30/b0, 32/c3 | `00 11` each | Failed status; payload invalid |
| 1-2.2 / 31 | 30/1c | `01 00` | Success, data 00 |
| 1-2.2 / 31 | 30/b0 | `01 80` | Success, data 80; not terrestrial lock |
| 1-2.2 / 31 | 32/c3 | `01 d0` | Success, data d0; not satellite lock |

The second runtime's customer support_feature is `8c`, versus `0c` on the
first. The differing bit's role remains unverified. Successful register access
establishes neither tuning nor MPEG-TS reception. No GPIO or I2C write was made
by this read-mode correction.

## Terrestrial diagnostic increment, 2026-10-08 afternoon

Same-default code agent resumed with user approval after its 20-minute timeout.
Help/invalid arguments opened no `/dev/bus/usb` nodes under strace; all 11
offline CTests passed after parent review corrections. Real RF remains separate.

Target secondary `1:31`, expected port `1-2.2`, with interface 0 claimed:
`asicen-frontend --device 1:31 --port 1-2.2 --timeout-ms 5000 power-on`
completed all 19 planned operations. Seven GPIO requests returned length 1;
probe read at a8/00 and demod 1c reads/writes returned length 2 with I2C status 1.
Mask 20 was only set; mask 40 was not addressed.

The following terrestrial `init` completed all 22 demod register writes but
failed at the first FC0012 bridge send, after two staging fills succeeded:
`c0:0d value=fe00 index=01c6 length=4` and
`c0:0d value=0503 index=0000 length=2` returned status 1.
`c0:0e value=0030 index=0000 length=5` returned 5 bytes with status 0.
The command stopped immediately (24 of 86 ops) and exited 1. Raw trace:
`evidence/raw/terrestrial-init-secondary.txt`. No tune/capture/card command
followed this failure. Bridge/power initialization is the current failing gate.

### Safe startup subset and real terrestrial lock

Secondary GPIO08 clear (`c0:08 value0800`, reply f7) and GPIO80 clear
(`c0:08 value8000`, reply77) did not change the failed FC0012 send. These
operations do not establish a physical role for either bit.

Primary power-on also initially failed I2C a8 and demod1c reads (status0).
Parent then sent primary `c0:08 valueBB27 index0 length1`, reply67. This is
the original DTV_Start value27/maskFB operation with sibling mask40 excluded;
LNB20 is set, not cleared. The next primary power-on completed all19 ops,
then terrestrial init completed all86 ops with all I2C status bytes1.
This observed dependency belongs to the combined startup subset; its physical
bit roles and fresh cold-start recovery are not individually established.

`asicen-frontend --device 1:29 --port 1-2.1 --timeout-ms 5000
--lock-timeout-ms 3000 --frequency-khz 557142 tune` completed its register
operations and polled demod b0 to `a9`, reporting `locked=yes`. Acceptance
increment2 (one real terrestrial lane tuned/locked) is PASS. Logs:
`evidence/raw/terrestrial-power-primary-after-startup-subset.txt`,
`evidence/raw/terrestrial-init-primary-after-startup-subset.txt`,
`evidence/raw/terrestrial-tune-primary-T27.txt`.

A 5-second lane1/endpoint82 raw capture then returned zero bytes, exit1.
The API-labeled DSC start/stop calls replied1, but emitted request07 then
request06 respectively (actual stop then start under the corrected mapping).
No USB/output errors were reported; zero data was treated as failure. This is
not a valid corrected-DSC capture result. No MPEG-TS or B-CAS success is
claimed.
Log: `evidence/raw/capture-primary-T27.txt`; sample is an empty local binary.

### CF experiment and controller revision

Primary lane1 CF control read (`c0:04 value00c0 length2`) returned `01 04`.
Parent set PID boundaries41/43 to `1fff`, read both back, sent channel reset
`c0:09 value0001 length1`, and wrote CF40=`0b` (lane1 subcommandc0). Control
readback was `01 0b`. A subsequent 5-second capture still returned zero bytes
and exit1 under the reversed DSC mapping described above. CF40 was restored to04
and read back as `01 04`; this restores that register only, not the entire
device state.
Log: `evidence/raw/capture-primary-T27-after-CF.txt`.

Passive controller I2C register09 reads returned `00 04` at slave5a (failed,
payload invalid) and `01 1e` at slave4a (success). The original bBCardInit
decodes `(register09 & 3e) >> 1`, yielding controller type0f. This identifies
the controller revision path, not B-CAS card presence or a successful APDU.
No card command or controller reset pulse was executed in this observation.

Offline stream setup/capture fault tests now pass13/13. Parent review found
CF block chunk-address and reset-state questions; the new automatic stream
setup is pending correction before hardware use. RF lock remains confirmed,
while TS reception, satellite reception and card decoding remain unconfirmed.

### Corrected CF offsets, explicit reset0 diagnostic

Luna corrected CF read offsets00/20/40 and write offsets including skipped
zero chunks. The CLI requires an explicit reset-state; the original caller's
STnimControl[0] default is unresolved. Local build and15 CTests passed.

Primary b0 remainedA9 on a fresh lock read. A5-second capture with reset-state0
completed all CF transfers but again returned zero bytes and exit1. Raw and
log: `evidence/raw/raw-primary-T27-CF-offset-reset0.bin` and
`evidence/raw/capture-primary-T27-CF-offset-reset0.txt`.

Before this test CF40..44 read `04 00 20 1f ff`; after it read
`04 1f ff 1f ff`. Parent restored the five original bytes with CF40 write
`04c0/2000/len4` and CF43 write `1fc3/00ff/len3`, then confirmed readback
`04 00 20 1f ff`. The reset0 block's all-zero chunk skip left CF40 bit2 set;
this matches the reconstructed zero-skip path and does not establish the
bit's electrical role. No further output gate is assumed from the CF result.

The focused static scout traced STnimControl[0] to Tnim_Initialise/TC_Initialise:
the initializer returns output bytee9 for terrestrial local1, and its low bit
is stored at control byte0. Thus the original initialized terrestrial caller
passes fourth argument1. A second corrected-offset 5-second capture with
reset-state1 also returned zero bytes and exit1; log:
`evidence/raw/capture-primary-T27-CF-offset-reset1.txt`. The five CF bytes were
restored and read back again as `04 00 20 1f ff`.

A passive public-controller register05 read at slave4a returned `01 00`.
The observed register value00 alone does not establish its TS output semantics
or authorize an assumed enable sequence. No controller register write was made.

### Isolated controller-register candidate test

Parent then tested the source-derived initializer candidate05=`20` as a
bounded experiment, not a default driver setting. Register05 was read as00,
written via `c0:03 value054a index0020 length2` (I2C status1), then read back
as20. A5-second corrected-CF/reset1 capture still returned zero bytes and
exit1. Log: `evidence/raw/capture-primary-T27-controller05-20.txt`.
Parent restored05 to00 and confirmed readback00, then restored/read back
CF40..44 as `04 00 20 1f ff`. No GPIO/LNB/APDU operation accompanied this test.
The byte20 alone does not resolve the TS gate; its electrical bit meanings
and the full controller initializer remain unverified.

Parent subsequently executed only the controller-type0f public initialization
subset: GPIO value/mask ff/11,00/11,ff/11 with25/25/50ms delays, then read
4a/09=`1e`. GPIO replies were77,66,77; register05 remained00 and terrestrial
b0 remainedA9. Register05 candidate20 plus a5-second capture again yielded
zero bytes/exit1 (`evidence/raw/capture-primary-T27-controller-init.txt`).
Register05 was restored/read back00, GPIO mask11 restored/read back76, and
CF40..44 restored/read back `04 00 20 1f ff`. No key/APDU/LNB operation ran.

Kernel static review confirms lane1 bulk endpoint82 and four asynchronous URBs
queued before DSC in the original path. The current prototype instead starts
DSC before a synchronous read. This source-proven host ordering difference is
the next bounded diagnostic; its contribution to zero bytes is unproven.

The optional depth4 path built and passed16 local CTests, submitting four
libusb async transfers before DSC and cancelling/draining before release.
Both the5-second depth4-only trial and the depth4 plus public-controller init
and05=20 trial returned zero bytes/exit1. Logs:
`evidence/raw/capture-primary-T27-queued4.txt` and
`evidence/raw/capture-primary-T27-queued4-controller-init.txt`.
Controller05, GPIO mask11 and CF40..44 were restored/read back as above.
The capture duration bounds acquisition; terminal callback drain can extend
cleanup beyond that deadline to preserve transfer/buffer ownership.

Passive SlowdownIC version read atd2/00 returned `00 04` (failed status,
invalid payload), providing no live-device basis for SlowdownIC configuration.

### Shared demod initializer trial

The opt-in shared-demod plan adds only the original satellite-demod42 I2C
writes to32; local build/17 CTests passed. Parent first read/snapshotted all
42 original registers, then ran the shared terrestrial initializer. All
operations completed and557142kHz again locked with b0=A9.

Both the depth4 shared-demod-only trial and the shared-demod plus public
controller initializer/05=20 trial returned zero bytes/exit1 after5 seconds.
Logs: `evidence/raw/terrestrial-primary-T27-shared-demod.txt`,
`evidence/raw/capture-primary-T27-shared-demod-queued4.txt`,
`evidence/raw/capture-primary-T27-shared-demod-controller-queued4.txt`.
Parent restored controller05=00, GPIO76 and the five CF bytes; restored all
42 satellite-demod registers and verified all42 readbacks matched the saved
snapshot. A final terrestrial lock read remainedA9. This establishes neither
satellite tuning nor TS output. No LNB enablement or card APDU occurred.

Correction: the prior shared-demod trial used an incorrect satellite value
table. The source table is now matched to the exact 42-byte values slice
`.rodata 0x240..0x269` from TunerControl.o SHA-256
`26956331982fce11b4f1b9abbfe5439fcd46f82dca1b2a1499c75f429d83e84f`.
Therefore the earlier zero-byte result is not evidence about the corrected
table; it also does not establish that satellite initialization enables TS.

### Raw-read prerequisites and DSC state observation

Static inspection of the original open/init/stream path found no call to
`DTV_GenEncSeed` before the first raw USB read. The normal link-data software
transform follows that read. This does not establish the ASIC's internal
requirements or prove that returned bytes will already be ordinary TS.

Parent compared primary lane1 CF40..44 before and after the API-labeled DSC
start/stop sequence. All three reads returned `01 04 00 20 1f ff` (first byte
is the wrapper reply). That sequence emitted request07 then request06, which
the corrected mapping identifies as stop then start; cleanup's repeated
API-labeled stop also emitted actual start request06. The final terrestrial
lock read returned `01 a9`. This does not show CF behavior with DSC correctly
started and stopped.

### Optional CF40 filter-start capture diagnostic

`asicen_frontend --filter-start --queue-depth 4 capture` is an opt-in,
local-1-only diagnostic. It snapshots CF40 before stream setup, applies the
source-observed selector-on bits before queue/DSC start, then reads CF40 again
and sets the source-observed filter-start bit after DSC succeeds. It restores
the original CF40 byte after stopping DSC and draining queued transfers on all
capture outcomes. The option is disabled by default.

Parent's5-second primary T27 trial used the then-reversed DSC mapping and
returned zero bytes/exit1; it is not valid corrected-DSC counterevidence. A
second trial combined the same post-start filter operation with controller05
candidate20, also under the reversed mapping; it is likewise not valid
corrected-DSC counterevidence. Logs:
`evidence/raw/capture-primary-T27-post-start-filter.txt` and
`evidence/raw/capture-primary-T27-post-start-filter-controller20.txt`.
After the first trial CF40 read back04, confirming the diagnostic restored
that byte; PID boundaries still reflected ordinary stream setup. Parent
restored all five saved CF bytes after each trial. Final readbacks were
CF40..44=`04 00 20 1f ff`, controller05=00, GPIO76 and terrestrial b0=A9.
Because both trials used the reversed DSC mapping, they do not resolve
zero-byte acquisition with DSC correctly started. Local build and17 CTests
passed, including new ordering, restoration-failure and CF-response semantics cases.
No satellite tuning, LNB enablement or card APDU occurred.

### First corrected-DSC capture trial

After correcting start to06 and stop to07, parent repeated the previous
5-second primary T27 trial with reset1, local1, queue-depth4 and filter-start.
No GPIO, controller, crypto or transfer-size setting was changed for this
comparison. Stream setup completed; capture reported zero-bytes, bytes0,
exit1 and close_ok=yes. Log:
`evidence/raw/capture-primary-T27-corrected-DSC.txt`; the raw output is empty.
This establishes no received sample under that corrected configuration;
it does not establish zero USB payload across terminal callback drain.

CF40 read back04 after capture. Parent issued the corrected stop07 again,
restored the saved five CF bytes and verified CF40..44=`04 00 20 1f ff`,
controller05=00, GPIO76 and terrestrial b0=A9. DSC correction is necessary
for matching the original API but did not yield data in this first bounded
trial. Other earlier register-combination trials have not been repeated with
the corrected DSC requests. Local build and17 CTests passed, with independent
literal USB setup checks for start/stop on both local lanes.

### Callback-observed controller05 comparison

Optional `--queue-diagnostics` now retains up to256 callback events and
uncapped counters, recording slot, submit generation, monotonic time, phase,
raw libusb status, requested length and actual length. Phases are0 Normal,
1 StoppingDsc and2 CancelDrain. It also records normal-loop handoffs before
output handling, pending/ready counts before stop and cancellation return
codes. Diagnostics print to stderr after queue cleanup. No transfer size,
timeout, DSC/filter operation or cleanup order was changed.

Parent compared two5-second primary T27 trials using corrected DSC06/07,
reset1, local1, queue-depth4 (four4096-byte reads) and filter-start:

| Observation | controller05=00 baseline | controller05=20 comparison |
| --- | --- | --- |
| Raw file bytes / exit | 0 / 1 | 0 / 1 |
| Normal handoffs / bytes | 0 / 0 | 0 / 0 |
| Pending / ready before stop | 4 / 0 | 4 / 0 |
| Callbacks Normal / StoppingDsc / CancelDrain | 0 / 0 / 4 | 0 / 0 / 4 |
| Callback actual bytes, all phases | 0 | 0 |
| Callback status | four CANCELLED (raw3) | four CANCELLED (raw3) |
| Requested / actual length, each slot | 4096 / 0 | 4096 / 0 |
| Cancel return, each slot | 0 | 0 |
| Event overflow / duplicate callbacks | 0 / 0 | 0 / 0 |

Before comparison, terrestrial lock readA9 and controller05 read00. Parent
wrote `c0:03 value054a index0020 length2` and confirmed05 read20, then ran
the same capture command. No additional GPIO, seed, satellite or SlowdownIC
operation was added. Output20 is derived from original initializer flags;
this experiment tests only that register difference, not the full original
polling sequence. It did not produce an observable sample or nonzero callback
length, and does not establish that output20 is unnecessary in other states.

Logs: `evidence/raw/capture-primary-T27-observed-controller00.txt` and
`evidence/raw/capture-primary-T27-observed-controller20.txt`; both raw files
are empty. Terminal callback payload discarded by the existing drain path
does not explain these two outputs: all observed actual lengths were zero.
This is a libusb observation, not a USB bus-analyzer capture.

Parent sent corrected stop07, restored/read back controller05=00 and
CF40..44=`04 00 20 1f ff`; final GPIO76 and terrestrial b0=A9. Local build
and17 CTests passed, including callback accounting and handoff cases.
The next proposed independent comparison is the source-observed post-lock
FilterReset/FilterONOFF timing; that comparison has not been performed.

### Extra FilterReset/FilterONOFF timing A/B

The optional `--filter-repeat before|after` diagnostic retains the existing
setup and adds exactly one operation pair P: fresh-block
FilterReset(local1, block_rmw1, state1), then fresh CF40 RMW OR03.
It requires reset1/local1/queue-depth4/filter-start. Both variants check
terrestrial lock before the variant branch and immediately after post-start
bit3. P obeys the existing acquisition deadline. No fixed wait, Clean buffer
substitute, additional PID-boundary setting, controller write or GPIO action
is included. The original polling branch is conditional; this experiment
does not assert that P is always required by the official driver.

| Variant | Operation order after ordinary setup and selector-on |
| --- | --- |
| A (`before`) | lock, P, four submits, DSC06, bit3, lock, acquisition |
| B (`after`) | lock, four submits, DSC06, bit3, lock, P, acquisition |

Before ordinary setup the diagnostic snapshots CF00..44 (69 bytes).
After stop/drain it writes every saved chunk, including all-zero chunks,
and reads back all69 bytes for equality. This full-block restoration is
specific to filter-repeat; ordinary filter-start still restores only CF40.

Parent held controller05=20 throughout the paired A/B run, with T27,
4x4096-byte transfers and5-second deadlines. Results:

| Observation | A before | B after |
| --- | --- | --- |
| Lock before / after bit3 | A9 / A9 | A9 / A9 |
| P result / duration | ok / 160858 us | ok / 156318 us |
| CF40 before / after P | 07 / 07 | 0f / 0f |
| CF41..44 before / after P | 1f ff 1f ff / same | 1f ff 1f ff / same |
| File bytes / exit | 0 / 1 | 0 / 1 |
| Normal handoffs / bytes | 0 / 0 | 0 / 0 |
| Pre-stop pending / ready | 4 / 0 | 4 / 0 |
| Callback counts Normal / StoppingDsc / CancelDrain | 0 / 0 / 4 | 0 / 0 / 4 |
| Callback actual bytes, all phases | 0 | 0 |
| Each terminal callback | CANCELLED, requested4096, actual0 | same |
| Each cancel return | 0 | 0 |

Logs: `evidence/raw/capture-primary-T27-filter-repeat-paired-before.txt`
and `evidence/raw/capture-primary-T27-filter-repeat-paired-after.txt`.
All69 CF bytes matched the original snapshot after each capture; controller
read20 and lockA9 between runs. At final cleanup parent sent corrected
stop07, restored controller05=00 and all69 CF bytes, and read back
CF00..3f allzero, CF40..44=`04 00 20 1f ff`, controller05=00, GPIO76,
terrestrial b0=A9. No LNB, satellite tune or card APDU operation ran.

An initial A-only attempt also reported zero, but parent mistakenly tested
only the last20 log lines and missed its first lock line. The wrapper
restored state and stopped before B. The paired comparison above was rerun
with both lock lines checked from the full logs and controller20 held
constant. The initial log is retained separately as
`evidence/raw/capture-primary-T27-filter-repeat-before.txt`.

This pair's placement alone did not produce a received sample or nonzero
libusb callback length in the tested state. It does not rule out other
startup prerequisites or establish that the conditional official polling
path has been fully reproduced. Local build and17 CTests passed, including
A/B order, common lock checks, fresh-bit preservation, transfer-length and
full-block restoration cases. Existing test expectations were unchanged.

### Corrected shared satellite-demod table comparison

Parent independently extracted exactly42 bytes at .rodata240..269 and
42 register bytes at280..2a9 from TunerControl.o. Its SHA-256 matches
`26956331982fce11b4f1b9abbfe5439fcd46f82dca1b2a1499c75f429d83e84f`.
The implementation's42 register/value pairs now match those slices exactly;
19 previously misaligned values were corrected. The test fixture now uses
source literal bytes with inferred initializer length asserted42, rather
than retaining the incorrect expected list. Other frontend/tune operations
and retry RMW behavior were not changed.

Parent first saved all42 satellite-demod registers, then compared ordinary
terrestrial initialization/T27 tune with the corrected shared-demod
initialization/T27 tune. Neither condition replays the incorrect table.
Both completed initialization and lockedA9. After each lock, controller05
was set/read20, followed by identical5-second capture: reset1, local1,
four4096-byte reads, corrected DSC06/07, filter-start and queue diagnostics.
Filter-repeat was disabled for both conditions. Both returned zero file
bytes/exit1, zero normal handoffs, pending4/ready0 before stop, and four
CancelDrain callbacks with status3/CANCELLED and actual_length0. No new
GPIO sequence, initial773143kHz tune, gain polling or SlowdownIC write was
added. This comparison concerns the shared initialization set, not the
causal effect of one satellite register.

Post-shared readback status succeeded for all42 registers. Values matched
the corrected table for41; reg01 was written90 and read10. That readback
difference is recorded without assuming the register's electrical semantics.
The following hex strings list42 bytes in the same register order:

```text
registers: 010304060708090a0c0d0e0f101112131415171a1b1c1d1e1f2038393b5152535a5b85878d8ea3a4a5a6
baseline:  10000000410000ff59f2f050b200300000000000000000000000401090c08a132e235900000077000004
shared:    10000200410000ff59f2f050b200308000000000000000000000401090b089b32dd36904000011004004
```

Logs: `evidence/raw/terrestrial-T27-terrestrial-only.txt`,
`evidence/raw/terrestrial-T27-corrected-shared.txt`,
`evidence/raw/capture-primary-T27-terrestrial-only.txt`, and
`evidence/raw/capture-primary-T27-corrected-shared.txt`. The two raw captures
are empty;42-register snapshots/readbacks are retained as local JSON.

Parent stopped with request07, restored all42 satellite registers and
verified exact equality to the saved values; restored all69 CF bytes and
verified exact equality. Final controller05=00, GPIO76 and terrestrial
b0=A9. The corrected shared table did not yield observable bulk data in
this tested state; it remains a required implementation correction and
does not establish whether shared initialization is unnecessary. Local
build and17 CTests passed. No satellite tuning or card APDU occurred.

### Official module load/unload in the isolated CentOS guest

On2026-10-08 around21:45 JST, user provided an SSH route through Latitude
to localhost port2263. Guest observations: hostnameDG-STK5S, CentOS6.3 Final,
kernel`2.6.32-279.el6.x86_64`. No USB passthrough was configured.
`/sys/bus/usb/devices` was empty and `/dev/bus/usb` absent; lsusb returned
`unable to initialize libusb: -99`. This is not a receiver-driver failure.

Parent copied the two official modules to guest
`/tmp/asicen-driver-check-20261008/`, verified their vermagic matched the
running kernel, and performed a USB-unconnected load/unload check:

| Module | SHA-256 | Load / unload |
| --- | --- | --- |
| loader.ko | 10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0 | success / success |
| as11usbdtv.ko | a51e1278d925b6f45720f51f7b860c4523166c465721bca2792c33b62e1bb3c7 | success / success |

Both vermagic values are
`2.6.32-279.el6.x86_64 SMP mod_unload modversions`.
Guest lsmod showed loader22890 and as11usbdtv24467, each with use count0.
Kernel messages confirmed registration of `ASICEN_USB_LOADER` and
`ASICEN_USB_DTV`, followed by deregistration on cleanup. A final lsmod check
confirmed neither module remained loaded. Vendor binaries were loaded only
as kernel modules in this explicitly supplied isolated guest, not in HAOS
or the Latitude host kernel.

An initial modprobe usbcore returned Module not found. The guest kernel
configuration has CONFIG_USB=y; `/sys/module/usbcore` and exported
usb_register_driver/usb_deregister symbols exist, so usbcore is built-in
and that modprobe attempt was unnecessary. No force-load options or module
installation into /lib/modules were used.

This verifies only kernel-module initialization and cleanup compatibility.
Device probing, firmware loading through the official driver, actual tuning,
bulk streaming and USB tracing remain unverified pending USB passthrough.
The receiver was not moved or manipulated by this guest check. No VM restart,
USB passthrough configuration or HA setting/service change was performed.

### Receiver USB passthrough into the running CentOS guest

On 2026-10-08, following the user's passthrough request, Latitude exposed
two loader devices (`1738:5211`) at physical USB paths `1-2.1` and `1-2.2`.
The running VM was direct QEMU 10.2.1, not a libvirt-managed domain.
Through its existing HMP socket `/var/tmp/centos63/mon.sock`, the following
devices were added without restarting the VM:

```text
device_add usb-ehci,id=asicen_ehci
device_add usb-host,id=asicen_primary,bus=asicen_ehci.0,port=1,hostbus=1,hostport=2.1
device_add usb-host,id=asicen_secondary,bus=asicen_ehci.0,port=2,hostbus=1,hostport=2.2
```

Guest ehci_hcd initialized the controller at PCI `0000:00:04.0`.
Guest lsusb then listed both `1738:5211` devices, at bus 001 addresses 002
and 003. Kernel logs reported ASICEN/AUSBDTV on guest ports `1-1` and `1-2`.
QEMU reported both devices at 480 Mb/s. The official loader and runtime
modules remained unloaded; firmware loading and receiver probing are still
unverified. Only the receiver's two USB functions were passed through.

This is live configuration for the current QEMU process, not a change to
its startup command. Host physical-port matching avoids pinning an address
or product ID, but firmware re-enumeration through QEMU remains untested.
Rollback via the same monitor: `device_del asicen_primary`, then
`device_del asicen_secondary`, then `device_del asicen_ehci`.

### Official loader/probe USB trace, before userspace initialization

On 2026-10-08 around 21:58 JST, capture was started before loading either
official receiver module. Physical power-on and the initial guest USB
enumeration had already happened and are not in this recording.
Guest CONFIG_USB_MON=y supplied usbmon after mounting debugfs; no usbmon
module was needed. Guest bus 1 contained only the virtual root hub and the
two receiver functions. A text capture covered both functions continuously.
Latitude simultaneously recorded full-snaplen USB pcap, initially filtered
to receiver addresses 99/100. That filter does not include later addresses.
After firmware re-enumeration, a separate pcap used device address >=99 on
bus 1; the observed webcam, Bluetooth, keyboard and physical hub were all
below that range and excluded. There is a host-capture gap between these
files; guest text capture remained active through it.

Loading official loader.ko produced, on each guest function, nineteen
request AB writes and a final request AC write, all with length 512,
wIndex 5399. All forty write completions reported status 0 / length 512.
The initial host pcap contains 92 events and tcpdump reported zero drops.
The later host pcap contains 34 events and zero reported drops.
Guest text contains 164 events; text payloads are truncated by usbmon, so
the host pcap is the source for full captured firmware payloads.

After the firmware transition, Latitude showed only primary 0b06:0005 at
physical port 1-2.1, address 102. QEMU retained stale guest device state.
Parent removed and re-added only asicen_primary/asicen_secondary through
HMP, with capture still running. Guest then enumerated primary runtime at
bus 1 address 4. The secondary physical function remained absent.
Loading as11usbdtv.ko bound interface 0 and created /dev/as11usbdtv0.
Interface 1 probe reported -12; this agrees with the official driver's
one-endpoint-interface rejection described in the continuation audit.

No official userspace initializer, tuning or stream start was executed.
There are no bulk submissions or nonzero bulk completions in this trace.
Static inspection found that the required TF_DTV_DevCreate -> DTV_Start
path clears GPIO bit 20, which TC_SetLNB uses as its LNB-on encoding.
Physical voltage/polarity remains unverified; parent requested authorization
before running this operation. bBCardInit performs decoder-chip I2C and GPIO
operations, not a B-CAS APDU read; Linux random-key wrappers are no-op stubs.

The sanitized guest trace, with firmware payload bytes omitted, is in
[the archived trace](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/hardware-traces/2026-10-08-official-loader-probe.usbmon.txt).
Raw guest text and both pcaps are preserved locally under
/config/.tools/asicen-work/official-trace-20261008/ and on the SSH machines.
Capture processes were stopped after this bounded recording. Both official
modules remain loaded, the runtime interface remains bound, and passthrough
remains configured. This is a loader/probe reference, not a successful
reception reference. A subsequent initializer run must restart recording
before its first call and retain the previously captured startup segment.

### Read-only GPIOEx and endpoint diagnostics with LNB excluded

On 2026-10-08 around 22:10 JST, the user explicitly excluded LNB operations
and noted that establishing the antenna voltage requires a physical tester.
Parent started a new guest bus-1 usbmon recording before diagnostic calls.
No GPIO write with a nonzero mask, GPIOEx write, clear-halt, USB reset or
SET_INTERFACE request was issued in this diagnostic experiment.

With as11usbdtv bound, device-recipient GPIO/GPIOEx/configuration reads
succeeded, but endpoint/interface-recipient requests failed with libusb -1.
Debug output identified submiturb errno16 (EBUSY), and kernel messages said
the process had not claimed interface0. Those failed requests did not appear
on usbmon; they are not evidence of an endpoint halt or a firmware refusal.

Parent temporarily unloaded as11usbdtv, claimed only interface0 for each
explicit guest bus/address, performed the reads, released/closed the handles,
then reloaded as11usbdtv. No SET_INTERFACE occurred in the captured trace.
During this recording, secondary also enumerated as loader address5 and
then runtime address6. This was not initiated by an explicit power command.

| Observation | Primary bus1/address4 | Secondary bus1/address6 |
| --- | --- | --- |
| GPIO request08, value/index0, length1 | ff | ff |
| GPIOEx request11, value/index0, length1 | 02 | 02 |
| endpoint82 GET_STATUS, actual length2 | 0000 | 0000 |
| endpoint81 GET_STATUS, actual length2 | 0000 | 0000 |
| GET_CONFIGURATION | 01 | 01 |
| GET_INTERFACE interface0 | 00 | 00 |

GPIO bit20 remained set in all observed reads. These are pre-initialization,
pre-tune observations; endpoint halt absence here does not establish its
state during a locked capture. GPIOEx reports pin levels; 02 means bit0 low
and bit1 high, without proving the output latch or electrical function.
No GPIOEx write was inferred from this observation.

The 194-event trace is preserved in
[the archived trace](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/hardware-traces/2026-10-08-readonly-diagnostics.usbmon.txt); its raw copy
is in the local official-trace-20261008 directory. Both receiver functions
are now runtime 0b06:0005 and the official runtime module is loaded again.
The bounded usbmon reader was stopped. No reception was attempted here.

### LNB-excluded official-library startup trial

On 2026-10-08 around 22:22 JST, parent ran the standalone guarded harness
preserved in [the archived harness](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/hardware-traces/official-trace-harness.cpp). This is a
modified-library comparison, not an unmodified official reception reference.
Linker wrapping removes bit20 from both GPIO value and mask and skips
GPIOEx writes. Host compilation and linking passed; disassembly of the
guest-linked executable confirmed the vendor GPIO/GPIOEx helpers called
the guards. An exhaustive offline check of value/mask/initial-state
combinations confirmed bit20 is preserved. Both USB traces started before
the first library call. No B-CAS APDU or seed-generation API was invoked.

The harness object was compiled with g++ -std=c++11 -O0
-fno-stack-protector -Wall -Wextra -Werror -c. Guest linking used:

```sh
gcc official-trace-harness.o libPlexLib_W3U3.a /usr/lib64/libstdc++.so.6 \
  -Wl,--wrap=_Z9ucSetGPIOhhi -Wl,--wrap=_Z11ucSetGPIOExhhi \
  -lpthread -lm -lrt -o official-trace-harness
```

The archive SHA-256 was
e1d68db2e09c584912a60363d89e56271ff3ad3b1bd0a319a7076fe914dad2c0.
The host-compiled object was linked using the guest GCC4.4.6 and its own
libstdc++.so.6, with both --wrap flags, -lpthread -lm -lrt. GCC4.4.6 uses
non-PIE linking by default; the newer -no-pie switch was not used. A first
timeout invocation rejected unsupported -k before executing the harness;
the subsequent timeout -s KILL 45s invocation completed normally in under
one second, returning exit1, without reaching its watchdog.

| Call/result | Observation |
| --- | --- |
| DevCreate /dev/as11usbdtv0 | 1 |
| AssignDevExt_0 | 1 |
| Init(local0, mode0, final mode byte0) | -2 |
| UnInit(local0) | -10 |
| DevClose | 1 |
| Guard totals | 12 GPIO calls, 2 with bit20 masked, 1 empty mask skipped, 1 GPIOEx skipped |

Local1 initialization, T27 retune and lock polling were not reached. Despite
DTV_Init's failure, TF_DTV_Init continued its polling/filter/bulk wrapper
path: local0 endpoint81 received four submissions of65536 bytes, followed
by four completions with status-2 and actual_length0 on cleanup. No
endpoint82 transfer or nonzero bulk completion was recorded. This observed
65536-byte path does not invalidate the separate official4096-byte path.

Static inspection identifies an early DTV_Init -2 branch at
DTV_Lib.o .text7250..72f5: it checks extension bytes476b..476e for VID0b06
and PID0004/0005. The observed USB descriptor matches, but the harness did
not read those internal bytes; the precise failed check is therefore not
confirmed dynamically. The earlier suggestion that GenEncSeed prepares
these fields was incorrect: the identified CalculateFinalKey path reads
identity inputs; it does not establish that preparation. Subsequent audit
identified the public TF_bGetCusInfo API as the missing preparation call:
Transform.o .text420..4b3 reads Customer_Info into extension+4768, including
the four identity bytes. The harness used for this recorded trial omitted
that API. No seed inputs or identity-cache patch was made.

The follow-up customer-info trial below supersedes this missing-prerequisite
hypothesis with an observed successful initialization.

After DevClose, parent sent DSC07 to both lanes, restored only non-LNB GPIO
bits with valueDF/maskDF, then read GPIOff and GPIOEx02. All234 guest trace
events were audited: zero request08 write masks selected bit20, and zero
request10 GPIOEx writes occurred. The host pcap has202 events and tcpdump
reported zero dropped packets. Secondary reset/re-enumeration caused by
the non-LNB startup sequence is included; final guest runtime devices were
primary address4 and secondary address8.

The sanitized trace is
[the archived trace](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/hardware-traces/2026-10-08-guarded-init.usbmon.txt); firmware and device
identity payloads were omitted. Raw guest text, host pcap and stderr remain
under /config/.tools/asicen-work/official-trace-20261008/ and on the SSH
machines. Capture readers were stopped. GPIO was restored; frontend/CF
register snapshots were not taken for this startup trial, so their full
restoration is not claimed. This run did not establish a usable frontend
initialization or a successful official reception reference.

### Customer_Info prerequisite trial (2026-10-08, 23:07 JST)

Following [the dedicated static audit](https://github.com/Khronos31/asicen-userland/blob/40343327409e38ba499032c5a54e044ac542bb0e/docs/reverse-engineering/official-init-customer-info-2026-10-08.md),
the only SDK preparation change was to call TF_bGetCusInfo after DevCreate /
AssignDevExt_0 and before Init. Existing Init arguments, LNB bit20 masking,
GPIOEx suppression, T27 frequency, and the five-second lock window stayed
unchanged. The API uses the SDK's documented 58-byte Customer_Info layout;
its header declaration is disabled under #if0, so the harness reproduces
the layout with size/VID/PID offset assertions. Only returned and cached
VID/PID are logged. There are no seed calls or identity-cache writes.

First, --customer-only performed the API call and DevClose without Init.
Both the guest usbmon and the host pcap show request c0/0c, value0, index0,
requested58, completion status0 and actual_length58. API return was1;
returned VID/PID and the read-only extension cache were both0b06/0005.
Parent then stopped both DSC lanes and restored GPIO with maskDF before
the full trial. The full trial repeated the same successful58-byte read.

| Full-trial call/result | Observation |
| --- | --- |
| DevCreate / AssignDevExt_0 | 1 / 1 |
| Customer_Info | 1; returned and cached VID/PID0b06/0005 |
| Init local0 / local1 | 1 / 1 |
| Requested T27 local1, 557142kHz, bandwidth6 | API returned1; later audit shows no RF retune occurred |
| Public lock checks during the five-second window | All0; later audit shows RF was not measured |
| UnInit local1 / local0 / DevClose | 1 / 1 / 1 |
| Harness exit | 1, because no lock was observed; watchdog not reached |

This resolves the observed harness Init=-2 with the normal public API.
It does not resolve the earlier userland zero-byte result after lock.
Subsequent audit showed the public tune and lock APIs short-circuited while
control+30d60 remained1; this trial cannot establish RF unlock at T27.
Init now emits its own SysCtrl reads and frontend
configuration, unlike the previous failed trial. Guest trace records four
65536-byte submissions each to endpoint81 and82; all eight completions
are cleanup cancellations, status-2 and actual_length0. The host capture
shows one pending bulk transfer per endpoint; its two bulk completions
also have status-2 and length0. StreamDataRead remains omitted because its
ioctl has no proven bounded timeout. No valid TS was obtained.

The combined two-phase guest trace contains1450 events. Every GPIO write
mask excludes bit20; no GPIOEx request10 occurred. Full-trial guard totals
were40 GPIO calls,17 bit20 masks removed,16 entirely skipped calls, and2
GPIOEx calls suppressed. After DevClose, parent sent DSC07 to both lanes,
restored only non-LNB GPIO bits using valueDF/maskDF, and read GPIOFF /
GPIOEx02, matching the initial snapshot. Voltage was not measured. Full
frontend/CF restoration was not performed or claimed. Final guest devices
were primary address4 and secondary address10.

The host pcap contains1430 events, with zero dropped packets. Captures were
started before DevCreate and stopped after restoration; no capture reader
or harness process remains. The sanitized combined trace is
[the archived trace](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/hardware-traces/2026-10-08-customer-info-init.usbmon.txt); EEPROM identity
and Customer_Info payloads are omitted. The 20 firmware-loader submissions
retain usbmon's truncated payload snippets. Full raw text,
pcap and stderr remain under
/config/.tools/asicen-work/official-trace-20261008/ and on the SSH machines.

The following is the historical build command at
[snapshot 02282ef](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/hardware-traces/official-trace-harness.cpp),
using the extracted SDK headers on the modern host and linking on the isolated
CentOS guest. Its recorded source path is not part of the current tree:

```sh
g++ -std=c++11 -Wall -Wextra -Wpedantic -Werror -fno-stack-protector \
  -I out/linux64/ReleaseToCustomer_64bit_130109_2 \
  -c docs/hardware-traces/official-trace-harness.cpp \
  -o official-trace-harness.o
gcc official-trace-harness.o libPlexLib_W3U3.a /usr/lib64/libstdc++.so.6 \
  -Wl,--wrap=_Z9ucSetGPIOhhi -Wl,--wrap=_Z11ucSetGPIOExhhi \
  -lpthread -lm -lrt -o official-trace-harness
sudo timeout -s KILL 45s ./official-trace-harness /dev/as11usbdtv0 ff --customer-only
# Check USB completion status0/length58 before running the full trial.
sudo timeout -s KILL 45s ./official-trace-harness /dev/as11usbdtv0 ff
```

Run restoration after DevClose (also after watchdog termination), rather
than interpreting --customer-only or the watchdog as device restoration.

### Public readiness gate and actual RF retune (2026-10-08, 23:30 JST)

The public frequency API's return1 was not evidence of a T27 retune:
DTV_SetTunerFreq .text6755..6768 returns1 after400ms when control+30d60
is1. DTV_TunerLockCheck .text5017..5028 likewise returns0 without reading
RF when that byte is nonzero. TF_DTV_Init sets it to1. The previous trace
contains only initial773143kHz PLL settings, not T27 settings. The earlier
description of that trial as RF no-lock is therefore corrected above.

An opt-in --rf-diagnostic trial called the official lower-level
Tnim_AcquireFrequency / Tnim_IsLocked components on the validated real
local1 control object. It left the readiness gate1 and supplied no seed,
key or direct flag write. Actual T27 PLL writes appeared;47 direct RF
polls returned1 in41 cases and0 in6, including the last poll. Demod b0
reachedA9. RF lock was observed, but uninterrupted lock and valid TS
were not established. All eight guest bulk completions had cancellation
status-2 and length0. Init/UnInit/DevClose succeeded, and non-LNB GPIO
restoration read backFF/GPIOEx02. No LNB mask or GPIOEx write was sent.

See [the analysis and bounded trial](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/reverse-engineering/official-tune-readiness-gate-2026-10-08.md)
for API addresses, the lower-level call's side effects and limitations,
trace counts, preservation locations and the still-unresolved supported
public receive-chain preparation. This trial does not reproduce the full
official application sequence or resolve the original locked zero-byte case.

### First official SDK TS capture (2026-10-08, 23:55 JST)

The next bounded trial supplied source-derived valid inputs to the public
TF_DTV_GenEncSeed API. Key2 row0 selected the normal read transform,
cleared the readiness gate, and allowed public T27 tune/lock followed by
38,420,244 bytes in20seconds. ffprobe recognizes four programs and their
video/audio PIDs. Key1 instead selected an application AES output path:
its188-byte framing was not sufficient evidence of usable payload.

The raw Key2 file has damaged startup packets within its first128packets;
the remaining204,235packets have no sync errors or TEI. This establishes
an official-SDK receive reference, not completion of the libusb userland
implementation or B-CAS/recisdb decoding. No LNB-mask/GPIOEx writes were
sent; both UnInit calls and DevClose succeeded, and restoration read back
GPIOFF/GPIOEx02.

Payload integrity is not yet clean: after the startup region,187 complete
PAT and423 PMT sections pass CRC, but15 complete PMT sections fail
validation, other sections are truncated, and20 CC jumps remain. Do not
interpret zero sync/TEI errors as zero loss or claim the initial128packet
exclusion repairs the entire recording.

See [the first-TS report](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/reverse-engineering/official-first-ts-2026-10-08.md)
for the public API sequence, transform distinction, validation, invocation,
trace limitations and retained artifacts. Full seed traces and TS remain
local; only code and non-key diagnostics are committed.

### W3U3 LNB 15V open-circuit measurement (2026-10-10)

Performed on the Linux glibc x86_64 release candidate while validating
release 0.1.0. The W3U3 satellite input was isolated from the building
wall distribution (open circuit); no antenna was connected, so the
satellite tune could not lock.

Method: `asicend --usb-path 1-2.1 --usb-path 1-2.2 --model w3u3
--allow-lnb-power --instance rcval`, then `asicen-ts --channel BS01_0
--lnb-voltage 15`. A multimeter was left attached to the satellite
input terminal and the LNB-on window was timed by the user.

Observed (manual multimeter reading):

- 0 V with no `--lnb-voltage` request.
- Immediately after the 15 V request began, the terminal measured
  15 V and stayed near 15 V for about 8 seconds.
- The satellite tune then failed to lock (no signal), which triggered
  the transactional LNB rollback; the software switched the LNB supply
  off.
- After the supply was switched off, the terminal voltage decayed
  slowly toward 0 V over about 90 seconds. The decay shape is
  consistent with the on-board capacitor discharging; the software
  did not hold the line at an intermediate voltage.

Interpretation: this matches the design (and the Q3U4 record from
2026-09-13) where 15 V is sustained while a tune is in progress and is
switched off on tune failure (`rollback_tune_power`). The difference
between devices is the width of the tune window and the board
capacitance, not the LNB transaction logic. The 15 V output itself is
working on the W3U3; only the open-circuit retention time differs
between models.

### Deterministic LNB 15 V window: `--tune-timeout-ms` (2026-10-10)

The width of the 15 V output window is the tune continuation time, which
is given by `asicen-ts --tune-timeout-ms`. This was confirmed on both
devices with the same explicit timeout.

- W3U3, `--tune-timeout-ms 30000`: the satellite input held 15 V for
  the full 30 s tune window, then the transaction rollback switched the
  supply off. The terminal voltage then decayed slowly through the
  on-board capacitor discharge; the user confirmed it reached 0 V.
- Q3U4, `--tune-timeout-ms 30000` (separate session): the input also
  held 15 V for 30 s, then dropped to 0 V immediately (no visible
  capacitor discharge).

Conclusion: the 15 V sustain window equals the tune timeout
(`--tune-timeout-ms`), independent of device and identical between
px4-ts and asicen-ts. The post-off voltage decay (instant vs slow
on-board capacitor discharge) is the only hardware difference between
the Q3U4 and the W3U3.

### Full-channel regression, representative machine (2026-10-10)

Representative row: `asicen-userland-0.1.0-linux-glibc-x86_64` on
AnduinOS x86_64 (PX-W3U3, primary receiver 1 for terrestrial,
receiver 0 for satellite). Satellite input is fed from the apartment
building wall distribution; LNB was kept at 0 V (no feeding toward the
shared wall equipment). Each channel ran a finite-length 10 s capture
through the release-candidate `asicen-ts`; all channels reported
sync-errors=0, tei=0, continuity-errors=0, queue-drops=0, usb-errors=0.

- Terrestrial (13 channels): T16, T17, T19, T21, T22, T23, T24, T25,
  T26, T27, T30, T31, T32.
- BS (9 channels): BS01_0, BS03_0, BS05_0, BS09_0, BS13_0, BS15_0,
  BS19_0, BS21_0, BS23_0.
- CS (11 channels): CS2, CS4, CS6, CS8, CS10, CS12, CS14, CS16, CS18,
  CS20, CS22.

Total 33 channels, all error-free. T30/T31/T32 failed once on the first
10 s pass and succeeded on retry; the first-pass failure was a transient
reception interruption, not a further retest.

### Short regression physical items, representative machine (2026-10-10)

The same linux-glibc-x86_64 candidate and PX-W3U3 were used for the
physical short-regression items: card remove/reinsert, USB detach during
capture, and recovery after reinsertion.

**Card remove/reinsert (internal B-CAS).** The daemon was left running
throughout. With the card inserted, `card-status` reported
`present=yes ... reader-generation=1` and `card-atr` returned
`3b:f0:12:00:ff:91:81:b1:7c:45:1f:03:99`. After the user removed the
card, `card-status` reported `present=no ... reader-generation=2`;
`card-atr` and `card-apdu 90:00:00:00` both returned `NO_CARD`
(exit 9). After the user reinserted the card, `card-status` reported
`present=yes ... reader-generation=3`, `card-atr` returned the same ATR,
and `card-apdu` returned `6d:00` (invalid-INS SW, i.e. the command
exchange succeeded). Generation advanced 1 -> 2 -> 3 and ATR recovery
confirming the daemon detects absence and reinsertion without restart.

**USB detach during capture.** While a 60 s T27 capture
(`asicen-ts ... --duration-seconds 60`) was streaming, the user
unplugged the W3U3 USB cable (one cable carries both USB functions).
The client exited in bounded time with `asicen-ts: DISCONNECTED` after
writing 217,521 packets / 40,893,948 bytes, and no asicen-ts or asicend
process remained. No IPC control socket remained in the runtime
directory. The daemon itself was not running afterwards (also ended
cleanly); when the device was re-enumerated, the old daemon did not
auto-recover.

**Recovery via cold start after reinsertion.** After reinsertion the
devices re-enumerated as loaders (`1738:5211` x2), as expected after a
cable pull. Starting the same candidate daemon with
`--firmware firmware/asicen-loader.bin` completed the loader transfer
for 2 functions, both USB functions returned to runtime (`0b06:0005`),
and the daemon reported ready with receivers=2. Verification after
recovery: terrestrial T27 (28,736 packets, sync/tei/cc = 0),
satellite BS01_0 at LNB 0 V (85,884 packets, sync/tei/cc = 0), and the
internal card ATR restored (same ATR as before). (A brief satellite 0
packet period after the operations was traced to the satellite input
becoming unconnected; the user reconnected it and reception resumed.)

### 2-hour soak, representative machine (2026-10-10)

Same candidate (`asicen-userland-0.1.0-linux-glibc-x86_64`) and PX-W3U3
on AnduinOS x86_64. A single continuous T27 capture
(`asicen-ts --receiver 1 --channel T27 --duration-seconds 7200`) ran
from 20:12:52 to 22:12:47 JST (2 hours), streaming to disk.

- TS: 14,135,249,624 bytes / 75,187,498 packets.
- Counter summary: sync-errors=0 tei=0 continuity-errors=0
  queue-drops=0 usb-errors=0 empty-intervals=0.
- daemon FD/RSS: start FD=16 / RSS=2536 kB, end FD=14 / RSS=2552 kB
  (no monotonic growth; no fd/rss leak observed).

Notes: the first 2-hour attempt streamed to /tmp, which is a tmpfs
(3.8 GB) on this host; it filled and the capture stopped after about
18 min (1,976,528,896 bytes). The representative run above streamed to
disk instead. This failed tmpfs attempt is recorded as the reason the
soak output target is a disk path, not a soak failure of the release
candidate.

## 2026-10-13: official PX4 v0.2.1 pin and replacement 0.1.0 candidate

Recorded by dot. This section records cloud CI and artifact inspection only.
It is not a hardware result, release-note approval, or tag/publication approval.

[PR #10](https://github.com/Khronos31/asicen-userland/pull/10) was merged into
`release/0.1.0` after independent source review and successful final-head CI.
The portable source subset is pinned to official PX4 v0.2.1 commit
`dd1c1bea16a5871085d5f2a42c009364c1ec7345`. The complete import/adapter scope is
in [the source manifest](third_party/px4-userland/UPSTREAM.md).

### Fixed candidate identity

- Candidate source: `329a925bd524e19e945a9dc2140ef2b28926125c`
- Source tree: `18d82f51394dc3498414afbc3a628b9d557992bb`
- Candidate push run: [38338502324](https://github.com/Khronos31/asicen-userland/actions/runs/38338502324), attempt 1
- Candidate `release-candidate` artifact: [11785698350](https://github.com/Khronos31/asicen-userland/actions/runs/38338502324/artifacts/11785698350)
- Candidate `release-provenance` artifact: 11786067895
- Independent workflow-dispatch run: [38338575743](https://github.com/Khronos31/asicen-userland/actions/runs/38338575743), attempt 1
- Reproduction `release-candidate` artifact: [11786281562](https://github.com/Khronos31/asicen-userland/actions/runs/38338575743/artifacts/11786281562)
- Reproduction `release-provenance` artifact: 11786556425

Both runs used the same source SHA/tree and `release/0.1.0`, completed all
20 jobs successfully, and had no pull-request source context. Run/attempt,
repository, head repository, workflow path and artifact bindings were checked.
The two candidate ZIPs are 10,489,443 bytes each; their Actions wrapper digests
are respectively `9ffbe7361213caa8ea63712e64f9ea7803532f2d7bcfa16a832cb714617163df`
and `0fb34d5a93bbdba642804704866b0fe2ef4693e9cea111966ad585247502bc84`.
Both downloaded wrappers matched the API digests. Wrapper bytes need not be
identical: the ten contained release files are compared independently.

### Offline checks and exact artifact reproduction

The frozen `release_verify.py` and `release_publish.py` helpers were used for
safe flat artifact extraction and checks of bundle inventories, outer and
inner checksums, source commit/tree, manifests, program modes, static libusb
identity, pinned firmware bytes/notices and Linux build-provenance bindings.
No archive executable was run during this inspection. The full release
verification/publication entry point was not invoked and no completed
hardware/notes record was fabricated.

All eight binary archives, the source archive and outer `SHA256SUMS` are
byte-identical across the two independent runs. Every complete file was
compared without timestamp, UUID, signature or metadata masking.

| File | SHA-256 (both runs) |
| --- | --- |
| `asicen-userland-0.1.0-android-aarch64.tar.gz` | `8df6d1a027b13b7670b3fec4df77e65ef80c4a0a4fb154568e46bb7b045feb11` |
| `asicen-userland-0.1.0-android-armv7a.tar.gz` | `3cf6bd6decf8d95803d8a4130990e5af49417fa3e8c1b08449474a408cf67a39` |
| `asicen-userland-0.1.0-android-x86_64.tar.gz` | `a507c276983448dedd5267c719fe3fa868313aacab41d095116123af30f5a667` |
| `asicen-userland-0.1.0-darwin-arm64.tar.gz` | `e079fc9dfc21de4cbc8093ce6d8dd662cf130ae1c2c89c12c706ab74a594bb8d` |
| `asicen-userland-0.1.0-linux-glibc-aarch64.tar.gz` | `1834904eb0aad0ed758ae5c27975d6ae380e6dfcfe05cfa1de2c04e5ea953c8e` |
| `asicen-userland-0.1.0-linux-glibc-x86_64.tar.gz` | `7cfeef13319ac3dfaf122c91346ccc7833345fe79e163a60367479063f100225` |
| `asicen-userland-0.1.0-linux-musl-aarch64.tar.gz` | `2e2c13d497c405e65d7267f1b689a9c95069c6bbed4bd35640a8011225055769` |
| `asicen-userland-0.1.0-linux-musl-x86_64.tar.gz` | `309ad632def1f43ffd2e35783da7252346901789de401d1755b0685bc15423e9` |
| `asicen-userland-0.1.0-source.tar.gz` | `442592b5d1807c9296c6352e921924b7ec7b78fb4c87687ba46ea2f04c64730f` |
| `SHA256SUMS` | `ab0ae630143aaddee823ced03dcca221d28b4451b7013301d8d7cca47d1e83ce` |

Recorded build inputs also match independently of the payload comparison:
the production Linux Docker digests and package/compiler versions, macOS
toolchain versions, Android NDK/compiler versions and explicit libusb source
digest were read from the paired job logs. All 20 paired jobs reported matching
runner-image versions. Representative effective inputs include:

- Alpine GCC 14.2.0-r6, CMake 3.31.7-r1, binutils 2.44-r3, pcsc-lite 2.3.3-r0.
- Debian GCC 10.2.1-6, CMake 3.18.4-2+deb11u1, binutils 2.35.2-2, pcsc-lite 1.9.1-1.
- macOS 14.8.9 / 23J631, Xcode 15.4 / 15F31d, SDK 14.5,
  Apple Clang 15.0.0 / clang-1500.3.9.4, ld-1053.12, CMake 4.4.3,
  Ninja 1.13.2, pcsc-lite 2.5.1 and pkgconf 3.0.6.
- Android NDK r27d, exact packaged `Pkg.Revision = 27.3.13750724` in all
  three ABI archives of both runs; Clang 18.0.4. Packaged binary-audit metadata,
  Linux libc archive hashes, target triples and binary/IFD hash bindings also
  match.
- libusb 1.0.30 archive SHA-256:
  `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`.

This is evidence for the observed build inputs, not a claim of a complete
compiler/runtime input SBOM. Windows build and native offline tests passed,
but no Windows archive is included in this ASICEN 0.1.0 candidate.

### Hardware handoff and unresolved issue

All hardware rows for this replacement candidate remain pending. Prior
candidate results above keep their original identity and are not transferred
to these changed binaries. Follow [release-validation.md](docs/release-validation.md)
for the eight-archive matrix, LNB measurements, short regression, representative
full-channel checks and six-hour aggregate soak schedule.

The shared CC checker changes observation and counting, not TS output bytes.
[Issue #7](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6108270376)
remains unresolved: saved old and new CS2 recordings contained actual TEI/CC
errors, including errors beyond startup. Earlier zero daemon counters and the
historical wording “all error-free” above must not be used as independent
proof of error-free TS. The stored recordings and the later offline analysis
take precedence for that question. [PR #8](https://github.com/Khronos31/asicen-userland/pull/8)
remains a separate, unmerged diagnostic and is not part of this candidate.

Begin with the representative host's required LNB/short-regression checks and
offline TS inspection. If CS2 remains unsuitable, record the failure and stop
short of declaring release acceptance; a code repair requires a replacement
candidate and the corresponding revalidation. Keep raw TS, card responses,
seeds and keys local; share counts, command/exit status, tool versions and
non-sensitive evidence locations. The release-note placeholders remain pending.

## 2026-10-13: receive-throughput fix and candidate f7352f4

Recorded by dot. This section supersedes the candidate identity and pending
status in the preceding `329a925` section; it preserves that section and the
earlier hardware history. A successful short capture, CI run or reproduction
check does not complete the eight-archive release gates.

### Production change and diagnostic lineage

[PR #11](https://github.com/Khronos31/asicen-userland/pull/11), production head
[`764c6f3fc7a72a77799059fa10b5b4dc8d7fcf45`](https://github.com/Khronos31/asicen-userland/commit/764c6f3fc7a72a77799059fa10b5b4dc8d7fcf45),
was merged into `release/0.1.0` as
[`f7352f4dcc71b364638279b774da3727fdec2ff7`](https://github.com/Khronos31/asicen-userland/commit/f7352f4dcc71b364638279b774da3727fdec2ff7).
The production change uses four 65,536-byte bulk-IN buffers and optimizes the
ASICEN mode-7 DES transform with fixed bit extraction, generated S/P tables and
decoder-local prepared key schedules. Independent scalar-oracle, lifecycle and
packet/chunk equivalence tests accompany the change. The official PX4 v0.2.1
pin `dd1c1bea16a5871085d5f2a42c009364c1ec7345`, shared packet/card contracts,
CLI, IPC and firmware stay unchanged. This is an ASICEN receive-path change,
not an update to the PX4 pin or a change to the four-packet sync probe.

The separate diagnostic lineage was
[`b7263c0a81ad632807f155aba2f6a6afb13ccf93`](https://github.com/Khronos31/asicen-userland/commit/b7263c0a81ad632807f155aba2f6a6afb13ccf93)
and fast-transform comparison
[`30486d0f5d33e3502fa06f2b1701be64e603ff87`](https://github.com/Khronos31/asicen-userland/commit/30486d0f5d33e3502fa06f2b1701be64e603ff87).
Both diagnostic builds used four 64 KiB buffers. Their receive recorder is
absent from the production source and binaries. The bounded captures and
timing analysis are detailed in
[Issue #7](https://github.com/Khronos31/asicen-userland/issues/7#issuecomment-6138823162).
The earlier probe-only PR #8 is a separate experiment and is not this fix.

### Fixed candidate and byte-identical reproduction

- Source: `f7352f4dcc71b364638279b774da3727fdec2ff7`
- Tree: `7d0091d0f3ff76601b21733d092a2b1716002291`
- Candidate: [push run 38358322625](https://github.com/Khronos31/asicen-userland/actions/runs/38358322625), attempt 1, [release-candidate artifact 11793508253](https://github.com/Khronos31/asicen-userland/actions/runs/38358322625/artifacts/11793508253)
- Reproduction: [workflow-dispatch run 38358427484](https://github.com/Khronos31/asicen-userland/actions/runs/38358427484), attempt 1, [release-candidate artifact 11794134216](https://github.com/Khronos31/asicen-userland/actions/runs/38358427484/artifacts/11794134216)
- Release-provenance artifacts: candidate `11793413315`, reproduction `11794188969`

Both runs completed all 20 jobs successfully on the exact same source/tree and
`release/0.1.0` ref, without pull-request source context. The two artifact ZIP
SHA-256 values are respectively
`76214c1a166f34ae79c96441c5f6b943a43190d72d5414135bb575d68b93e716`
and `e17ebe64897337208fc234ade150c46d36557c1abbf4bd039a814950b7d42925`.
These transport wrappers and run-specific provenance are not asserted to be
byte-identical. The ten distributed files inside were compared in full,
without timestamp, signature, UUID or metadata normalization:

| File | SHA-256 (both runs) |
| --- | --- |
| `asicen-userland-0.1.0-android-aarch64.tar.gz` | `dcae236704b7f863b67a086f9d582c4ba532edf8a224d3f693dd630b493fa5ee` |
| `asicen-userland-0.1.0-android-armv7a.tar.gz` | `16948d8e7713367a1adaa023b3286b74fe1eabe91beef444e4cf8b9aa14eacb7` |
| `asicen-userland-0.1.0-android-x86_64.tar.gz` | `d6df72f0ad51bc9b43e24755beae1359716e32a0b2a7a22f65ac781824b1b830` |
| `asicen-userland-0.1.0-darwin-arm64.tar.gz` | `78dfc393a9359950ba70a40be2ab33dade16cafb7b07eb4bdd2099811874218c` |
| `asicen-userland-0.1.0-linux-glibc-aarch64.tar.gz` | `43f45a4f33530df9e81b8bc7abc9ebf559465937fcca7ce042a11629bc585e6b` |
| `asicen-userland-0.1.0-linux-glibc-x86_64.tar.gz` | `0f3b106e78ab55f824009f56a7eda8abd5ed06af5c0cc817338cd1612fe5e2a8` |
| `asicen-userland-0.1.0-linux-musl-aarch64.tar.gz` | `f3f5dbbc1087b150db390d8ca0a8c7afcfdb58fe06aa6cc53ac10efe0b10fae1` |
| `asicen-userland-0.1.0-linux-musl-x86_64.tar.gz` | `b186076a4fa90522eee452f0b54b12dd40ed363a4dcbb87099a695066b9618ac` |
| `asicen-userland-0.1.0-source.tar.gz` | `68bae808503b89d53ebce93d04477c5659d61b3c2468cfd159a4869a50a0d35e` |
| `SHA256SUMS` | `755f455e628046fde9a9a83551708cb3f968bd0afb94d0931e7a2197c8b1f8f8` |

Offline inspection passed for exact bundle inventory, outer/inner checksums,
manifests, modes, source commit/tree, Linux provenance, static libusb and pinned
firmware identity. The source archive matched 402 repository blobs and their
modes, excluding the two firmware paths by policy, and included the verified
151-file pinned libusb source inventory. Archive executables were not executed
by this static audit. The successful CI jobs include their own offline/start
checks; they do not certify hardware. The release-helper workflow did not
trigger for this merge's path set, rather than running and passing anew. No
full publication entry point, completed hardware sidecar, tag or Release was
created by this audit.

### Observed build inputs and runner images

All 40 job logs were retained. The 20 job names pair between runs, and every
pair reported the same runner image/version:

| Runner image | Observed version in both runs |
| --- | --- |
| `ubuntu-24.04` | `20261004.327.1` |
| `ubuntu-24.04-arm` | `20261004.142.1` |
| `macos-14-arm64` | `20260831.0302.1` |
| `windows-2022` | `20261004.326.1` |

The production build logs and packaged provenance also record these matching
inputs, separately from the resulting archive-byte comparison:

- Linux static programs and musl IFD: `alpine:3.22`, observed digest
  `sha256:5291449c3df73caf6ed85e649dec1b9e818b39a5d8c871e97afc13e9cd5e8fa8`;
  GCC/G++ `14.2.0-r6`, musl-dev `1.2.5-r12`, CMake `3.31.7-r1`, binutils
  `2.44-r3`, pcsc-lite-dev `2.3.3-r0` and samurai `1.2-r7`.
- Linux glibc IFD: Debian 11 image digest
  `sha256:6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9`,
  Debian snapshot `20260825T000000Z` (`bullseye` and `bullseye-security`);
  GCC/G++ 10 `10.2.1-6`, CMake `3.18.4-2+deb11u1`, binutils `2.35.2-2`
  and pcsc-lite `1.9.1-1`. The glibc/musl archive qualification refers to the
  IFD; the packaged Linux command programs remain static musl binaries.
- macOS `14.8.9` / `23J631`, Xcode `15.4` / `15F31d`, SDK `14.5`,
  Apple Clang `15.0.0` / `clang-1500.3.9.4`, linker `ld-1053.12`,
  CMake `4.4.3`, Ninja `1.13.2`, pcsc-lite `2.5.1`, pkgconf `3.0.6`.
- All three Android ABIs: API 24, NDK `r27d`, packaged
  `Pkg.Revision = 27.3.13750724`, Clang `18.0.4`.
- Packaged libusb `1.0.30` source archive SHA-256
  `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`.
  The Alpine setup log also installs distribution libusb `1.0.28-r0`; that
  package version is not the identity of the pinned packaged libusb source.
- Loader: 16,384 bytes, SHA-256
  `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`.

Alpine's tag, hosted-runner labels and some package feeds remain mutable.
This proves the observed same-source push/dispatch pair and records its known
inputs; it is not a complete compiler/runtime SBOM or an indefinite-future
reproducibility promise. Windows passed build/offline checks but has no binary
in the eight-archive 0.1.0 distribution and no hardware certification here.

### Bounded CS2 diagnosis and production P0

These were individual 10-second captures on the same Latitude x86_64 host
(Intel Core i5-8265U, eight logical CPUs), PX-W3U3, primary receiver 0, CS2,
wall-fed satellite input and LNB 0 V. USB paths were `1-3.1` / `1-3.2`, runtime
`0b06:0005`, 480 Mbit/s. The comparison did not change USB/card/AC/antenna
connections or authorize LNB power. D0/C2/D1/P0 found the functions already
in runtime state and skipped firmware upload. No `USB_IO` error or residual
test process was reported; each capture daemon exited 0.

| Trial | UTC start–end, 2026-10-13 | Build / recording | Saved packets | TEI | Offline CC discontinuity + duplicate conflict | Stream sync errors | Client exit |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: |
| C0 | See retained trial logs | `b7263c0`, off | 208548 | 6 | 4146 + 60 | 577 | 8 |
| C1 | See retained trial logs | `b7263c0`, on | 200795 | 0 | 4261 + 132 | 558 | 8 |
| D0 | 06:19:57–06:20:09 | `30486d0`, off | 236345 | 0 | 0 + 0 | 0 | 0 |
| C2 | 06:21:35–06:21:47 | `b7263c0`, off | 218776 | 2 | 3322 + 6 | 584 | 8 |
| D1 | 06:23:20–06:23:32 | `30486d0`, on | 235996 | 0 | 0 + 0 | 0 | 0 |
| P0 | 06:35:47–06:35:59 | production `764c6f3`, no recorder | 235996 | 0 | 0 + 0 | 0 | 0 |

The 12-second wall-clock windows include capture startup/cleanup around the
requested 10 seconds. D0/D1/P0 also had zero discontinuity indicators, invalid
packets, queue drops and USB errors. D0/D1 PAT/PMT CRC checks were clean. C0
had three discontinuity indicators and C1 had one; neither had invalid saved
packets or queue/USB errors. Their bad captures remain evidence, despite
successful daemon cleanup.

Failed preparation attempts are retained separately: the first C0 endpoint
path was 114 bytes, exceeding Linux's 107-byte usable limit, and exited 2
before capture. A shorter private `/tmp/arx.XXXXXX` runtime allowed the actual
C0 trial. Before P0, an over-broad startup gate matched the benign message
`skipping upload` and aborted before capture. Correcting that gate led to
the single actual P0 capture above; the abort is not a second TS result.

C1 recorded 37,945,344 decoder-input bytes and all 582 metadata rows, without
dropped records, omitted raw bytes or invalid lengths. Its offline framer
replay matched the 578 compared rows and reproduced 558 sync losses, but
comparison stopped at `live_decoder_or_sink_failure`; it is not a full-period
match. The terminal sink refusal occurred at the finite capture boundary.
D1 recorded 44,367,872 bytes and all 681 rows; all 677 compared processing rows
matched, with zero replay sync loss and zero raw-header CC transition
candidates. Both recordings were finalized and passed the recorded
completeness checks. Valid callback bytes not passed to the decoder were
164,864 for C1 and 21,504 for D1; this includes termination/drain scope and is
not by itself proof of an unreported mid-stream discard.

| Recorded timing, milliseconds | C1 median | D1 median |
| --- | ---: | ---: |
| Decode excluding emit | 16.844630 | 2.093440 |
| Callback to resubmit | 34.218090 | 2.149696 |
| Ready to processing | 17.096341 | 0.001685 |

The maximum decode times were 30.562978 ms (C1) and 5.659542 ms (D1). The
fast/old/fast sequence and these timing changes strongly support inadequate
receive-processing headroom as the cause of the observed failure. They do
not locate a FIFO overflow or distinguish loss inside RF/device/USB. Callback
times are host observations, not bus arrival times. Raw adaptation fields
are still transformed, so raw CC transition candidates are not validated CC
errors. Replay verifies framing over its stated prefix with an always-accepting
sink, not transformed payload correctness or clean reception. Instrumentation
adds copies and clock reads; the recorder-off trials remain important controls.
Raw TS, raw USB-input captures, seeds, keys and card data remain local.

### P0 identity boundary and remaining gates

P0 used the pre-merge production source `764c6f3` from
[run 38356826018](https://github.com/Khronos31/asicen-userland/actions/runs/38356826018),
[Linux artifact 11793049429](https://github.com/Khronos31/asicen-userland/actions/runs/38356826018/artifacts/11793049429).
Its glibc-x86_64 archive SHA-256 is
`808ea441b75d1122e3bb000ee171a7f07e4b6da0e8df55fb31ebb6d3f586b9bd`,
which differs from the final candidate archive above. The commits have the
same tree, and exact archive-member comparison verified identical inventory,
modes and runtime bytes for the production and final Linux x86_64 archives.
In the glibc-x86_64 pair the verified SHA-256 values include:

| Runtime member | SHA-256 (P0 production and final candidate) |
| --- | --- |
| `asicend` | `222afe26fa7c895f773c1e89766eb1839853738bb93dd451e5cd26e6459e8099` |
| `asicen-ts` | `e91ff275470b54fa3d6d75f9aab53eb714d6f5f188989f11b8916ec67f3a475e` |
| `asicenctl` | `35cac5d8c6d754344f1efca72ec91608aec5b4895e3e1dcc5e34b9842a7c0e3c` |
| `ifd/asicen-userland-ifd.so` | `b0bbde616b2fee68cb94d6478bc0ae55e39db7e9597e4621d7a49fae4b7f1dd1` |
| `firmware/asicen-loader.bin` | `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8` |

The differing members are `manifest.json`, inner `SHA256SUMS` and the two
Linux build-provenance files, whose `source_ref` changes to the merge commit
with the corresponding manifest/checksum changes. Other provenance fields
and executable/library/firmware bytes remain equal. Thus P0 is a bounded
CS2 observation of matching runtime bytes, not a claim that the P0 and final
archives are identical or that every identical member was exercised by P0.

### Exact-candidate E03 33-channel sweep: one failure

The final `f7352f4` glibc-x86_64 candidate ran the existing 33-channel list on
the Latitude from 06:56:46 to 07:04:22 UTC on 2026-10-13. The bounded runner
finished `COMPLETED_WITH_FAILURES`, exit 1: 32 channels passed, and T27 failed.
The daemon exited 0 after SIGTERM, with no residual test process. This is a
completed sweep with a quality failure, not an all-channel pass.

- Terrestrial: T16, T17, T19, T21, T22, T23, T24, T25, T26, T30, T31 and T32
  passed; T27 failed.
- BS: BS01_0, BS03_0, BS05_0, BS09_0, BS13_0, BS15_0, BS19_0, BS21_0 and
  BS23_0 all passed.
- CS: CS2, CS4, CS6, CS8, CS10, CS12, CS14, CS16, CS18, CS20 and CS22 all
  passed.

T27 saved 109,806 packets and exited 8, with one offline CC discontinuity.
The saved-file validator reported zero sync errors, TEI, duplicate conflicts
and invalid packets; queue drops and USB errors were also reported as zero.
The stream log separately records `STREAM_END packets=109806 bytes=20643528
sync-errors=0 tei=0 continuity-errors=1 queue-drops=0 usb-errors=0
empty-intervals=0`, followed by `STREAM_END error=0`. Those stream counters and
the client exit 8 are both retained. Production source tracing confirms that
the backend [accumulates decoder/framer sync-loss deltas](https://github.com/Khronos31/asicen-userland/blob/f7352f4dcc71b364638279b774da3727fdec2ff7/userland/src/libusb_hardware_backend.cpp#L2437-L2451),
exposes them as source counters, and the stream service
[adds those counters](https://github.com/Khronos31/asicen-userland/blob/f7352f4dcc71b364638279b774da3727fdec2ff7/userland/src/hardware_stream_session.cpp#L646-L652)
to the final snapshot after joining the worker. Thus the recorded stream
sync 0 establishes no detected loss of already-acquired framing over consumed
input. It does not account for initial alignment/search discards:
[sync acquisition discards bytes without incrementing that loss counter](https://github.com/Khronos31/asicen-userland/blob/f7352f4dcc71b364638279b774da3727fdec2ff7/userland/src/ts_framer.cpp#L72-L84).
No separate raw-framer `sync_loss_events`, `discarded_sync_search_bytes`,
`pending_bytes` or first raw offset was saved; those separate observations
are unavailable, not zero. PAT CRC passed 101/101 sections and PMT CRC passed
232/232. Both validators exited 0, meaning analysis completed; their exit
status does not turn the nonzero continuity count into a quality pass.

Read-only inspection located the event at the beginning of the saved TS:
PID `0x0100` payload packets at packet indexes 0 and 2 have CC 5 and 7;
intervening packet 1 is PID `0x0900`, CC 12. None of the first 12 complete T27
packets is byte-identical to any of the last 5,000 saved T26 packets. That
negative match does not exclude hardware residue beyond T26's saved endpoint.
Source tracing found a fresh host queue, CC checker and decoder for the
capture, without a proven stale-host-data path; the DES transform preserves
the first four TS header bytes. These observations do not establish the
loss location or turn the failed channel into a pass. The original failure
is retained separately from the bounded follow-up below.

The bounded runner's SHA-256 is
`5b797113e0ff002f9e04062bbb4473a7b8cf09b38f3eb3940f27f35cc8f8d6bd`.
Commands, per-channel output and retained logs are under the host-local
`/home/yunomin61/asicen-0.1.0/rc-f7352f4-20261013T064810Z/evidence-parent/e03-allch-1`.
Raw TS stays on the Latitude. Satellite capture kept the existing wall feed
at LNB 0 V. This current setup overrides the generic 15 V example only for
these captures and does not authorize powering the wall input or waive the
separate LNB measurements.

### Bounded unchanged-candidate T26 → T27 → T27 follow-up

One bounded follow-up ran on the same final candidate and unchanged setup.
The input hashes matched the approved inputs. The runner reported `PASS` and
exited 0; the daemon exited 0 after SIGTERM, without a timeout or residual
test process. Each row was a requested 10-second capture:

| Trial | UTC start–end, 2026-10-13 | Packets | Bytes | Client exit | Valid PAT / PMT sections |
| --- | --- | ---: | ---: | ---: | ---: |
| `01_T26` | 07:25:26–07:25:37 | 111900 | 21037200 | 0 | 101 / 320 |
| `02_T27` | 07:25:39–07:25:50 | 108602 | 20417176 | 0 | 101 / 231 |
| `03_T27` | 07:25:52–07:26:03 | 110851 | 20839988 | 0 | 101 / 233 |

Both analyzers exited 0 for every capture; their content checks reported
zero CC discontinuities, TEI, duplicate conflicts, duplicate packets,
discontinuity indicators and invalid packets. No PAT or PMT CRC failure was
found. Every source `STREAM_END` reported zero sync errors, TEI, continuity
errors, queue drops, USB errors and empty intervals. The source-sync meaning
and unavailable initial-alignment/raw observations are as described above.

Runner SHA-256:
`7a8373bbce9d7a2d5e6f9d762e945e7cdf2ccb41d719cf638e9cd45cb3dcb3b0`.
Evidence is retained at
`/home/yunomin61/asicen-0.1.0/rc-f7352f4-20261013T064810Z/evidence-parent/e03-transition-t26-t27-t27-1`.
The original sweep remains **32 PASS / 1 FAIL**. This follow-up did not
reproduce the exact startup anomaly; it does not identify its cause, erase
that failure or override the full-channel release gate. It gives no basis
for blind startup-packet removal or a settings change. No further hardware
operation is recorded for this increment.

During the next normally planned validation, retain and inspect any startup
recurrence. If it recurs, a matched raw-input diagnostic could help locate
the loss; that would require its own agreed scope. This handoff adds no new
repeat count, soak duration or release gate.

### Release acceptance remains pending

Candidate building and same-source byte-identical reproduction are complete;
representative full-channel acceptance and overall release acceptance are not.
The eight-archive short-regression matrix, LNB measurements, card/USB physical
items and existing soak durations remain pending for this candidate. The
representative glibc-x86_64 and macOS soaks remain two hours each, the other
Linux archives 30 minutes each, and the three Android ABIs 10 minutes each.
Past candidate measurements do not complete these gates for changed binaries.
No formal release notes, completed sidecar, tag or Release is authorized or
completed by this record. Follow [the unchanged acceptance procedure](docs/release-validation.md)
and the separate user decisions required for release notes and tag/publication.

## 2026-10-13 cold-start sibling readiness fix

This section records a bounded repair verification on the PR #12 branch,
not acceptance of a newly fixed release candidate. The previous `f7352f4`
candidate and its failed trials above remain historical evidence. The changed
daemon requires a new candidate and the applicable release gates.

### Failure observed with cached USB identity diagnostics

On the Latitude PX-W3U3, diagnostic commit
`90778d0499da72b9c7ba39d0a945f46d82120389` completed firmware upload for both
USB functions. At claim time the primary was runtime `0b06:0005`, bus 1,
address 78, path `1-3.1`; the sibling was still loader `1738:5211`,
address 79, path `1-3.2`. The normal shutdown released the primary
successfully but the sibling returned `-4 / LIBUSB_ERROR_NO_DEVICE`, and the
daemon exited 7. A later read-only snapshot found the sibling runtime at
address 80. The cached claim/release logs add no USB transactions.

That trial ran from 12:35:53 to 12:35:56 UTC. Its T27 command mistakenly
included the satellite-only `--lnb-voltage 0` option and exited 2 before
reception. Thus it supplied USB ownership evidence but no TS or card test.
The failed harness trial is retained at
`/home/yunomin61/asicen-0.1.0/cold-diag-d3ega40w/trial-01`; it is not a
reception failure or pass. Earlier permission/preflight failures did not
execute another firmware/receive trial.

Static analysis found that the old post-upload wait checked only one
function, while ordinary ownership permitted a known loader sibling for
reservation. The diagnostic observation demonstrates the resulting stale
loader-handle path in this trial. It does not establish the exact historical
identity of every earlier shutdown error.

### Repair and offline verification

[PR #12](https://github.com/Khronos31/asicen-userland/pull/12), source
`b41ea41b7cfe8f175dd52b90e8eabfdd498037b0`, tree
`15118b2694c745bd5702d7a08db9aefe97eea6fe`, is based on `e58f316`.

- Track every successfully uploaded function by bus, full port path and
  expected runtime identity.
- Re-resolve every uploaded identity on every pass under one 10-second
  deadline, checking cancellation/deadline around each probe and before
  success. Bound sleeps by the remaining time.
- Require runtime identity when claiming an uploaded sibling. Preserve
  ordinary warm loader reservation and Android fd re-grant behavior.
- Retain cached claim/release diagnostics without changing USB return values
  or adding device communication.
- Keep pinned px4-userland v0.2.1
  `dd1c1bea16a5871085d5f2a42c009364c1ec7345` unchanged. These changes are
  ASICEN firmware/topology/backend handling, not a shared IPC/CLI contract.

The final integrated Release build passed 74/74 CTests on Latitude and
`git diff --check`. All 11 published Git blob hashes matched the tested local
files. Independent review found no remaining blocker after the deadline and
cancellation boundary fixes. Fake-probe tests exercise timing and identity
value forwarding; they are not physical USB descriptor tests. The existing
resolver performs VID/PID/bus/full-port matching, and fake ownership snapshots
test the strict sibling policy.

[Push run 38403124381](https://github.com/Khronos31/asicen-userland/actions/runs/38403124381)
and [PR run 38403136194](https://github.com/Khronos31/asicen-userland/actions/runs/38403136194)
each passed all 20 jobs. Ubuntu/Linux x86_64/Linux aarch64 passed 74/74 tests;
macOS passed 75/75, 3 targeted checks and 20 additional capture-lifecycle
iterations. Android, Windows offline builds/tests, source/relink, packaging
and artifact smoke checks passed. This does not certify other hardware
models or complete the eight-archive hardware matrix.

### One fixed-build cold-start trial

On 2026-10-13 the user reconnected the PX-W3U3 USB cable. The single fixed
trial ran from `13:23:15.754171Z` to `13:23:42.529641Z`; the requested
10-second T27 capture ran from `13:23:20.763501Z` to
`13:23:31.814794Z`. The host was the same Latitude/AnduinOS x86_64 E03
native libusb environment. Antenna/AC/card connections were unchanged.
The daemon did not receive `--allow-lnb-power`; no LNB voltage measurement
was performed, and disabled software permission is not a measured 0 V.

The package came from push run `38403124381`, artifact `11816188753`.
ZIP SHA-256:
`f7eaf94e7e564c1441fc0e911d04debde02fa77b06915921ac8cdd2e8d38cce3`.
The executed inner archive was
`asicen-userland-0.1.0-linux-glibc-x86_64.tar.gz`, SHA-256
`676abd3450f96b60d3abfdb6ca6ebbff289886ee16be7d11189a53e84b8f33b6`.
Internal checksums and manifest source matched before execution.

| Runtime member | SHA-256 |
| --- | --- |
| asicend | `1fa0e9d3de1abfc019c453d41c0a0db1653282578d4b0bbe83339f93d92f1041` |
| asicen-ts | `e91ff275470b54fa3d6d75f9aab53eb714d6f5f188989f11b8916ec67f3a475e` |
| asicenctl | `35cac5d8c6d754344f1efca72ec91608aec5b4895e3e1dcc5e34b9842a7c0e3c` |
| firmware/asicen-loader.bin | `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8` |

The harness verified both loader identities, all four hashes, no competing
ASICEN process and a new private runtime directory. Harness SHA-256:
`32ed771765428891217f4fbead15ef6d81466b8a10a1c56079833f54c217ee5f`.
Its earlier `ps -C` no-match handling was corrected to inspect a successful
`ps -eo pid=,comm=` listing; fake-main tests covered the normal and rejected
preflight paths. A denied evidence-directory write was retried through the
supported approval path before the one actual hardware trial.

Daemon options were `--model PX-W3U3 --usb-path 1-3.1 --usb-path 1-3.2
--instance cold-diag-v2 --runtime-dir /tmp/cd41 --firmware
<verified-package>/firmware/asicen-loader.bin`.
T27 used receiver 1, duration 10 seconds and a local output file, without a
satellite-only LNB argument. Ten card queries used `card-apdu 90:30:00:00:00`
after successful capture, followed by normal daemon SIGTERM shutdown.

| Observation | Result |
| --- | --- |
| Before firmware | Both loader `1738:5211`, addresses 85/86, paths `1-3.1`/`1-3.2`, 480 Mbit/s |
| Claim primary / sibling | Both runtime `0b06:0005`, addresses 88/90; both rc 0 |
| Release sibling / primary | Cached claim identities above; both rc 0 |
| After shutdown | Both runtime, addresses 88/90, 480 Mbit/s |
| Daemon / T27 / ten card client exits | All 0; no timeout or residual ASICEN process |
| Saved TS | 16,777,120 bytes; 89,240 packets of 188 bytes; no trailing bytes |
| Invalid packets / TEI / CC discontinuities / duplicates | 0 / 0 / 0 / 0 |
| PAT / PMT | 100/100 and 230/230 valid sections |
| Card transport and response format/length consistency | 10/10; status-word semantics not evaluated |

The offline validator used `Path.read_bytes()` on the entire saved TS;
`stat` and its `file_bytes` both report 16,777,120 bytes. It has no prefix
analysis limit. The client stderr summary agrees: `stream packets=89240
bytes=16777120 sync-errors=0 tei=0 continuity-errors=0 queue-drops=0
usb-errors=0 empty-intervals=0`. This saved size is exactly the packet boundary
below 16 MiB; the reason for that size is not established by these counts.
The requested duration was 10 seconds. These clean counts establish only
the recorded file and do not independently establish a full-bandwidth rate. Card transport/format success does not certify
all command semantics or a new adapter path.

Evidence is retained locally at
`/home/yunomin61/asicen-0.1.0/cold-diag-b41ea41b-evidence/trial-01/`.
The directory contains `daemon.argv.json`, `T27.argv.json`,
`card-01.argv.json` through `card-10.argv.json`, per-process UTC/exit files,
`sysfs-before.txt`, `sysfs-after.txt`, `subprocess-status.json`,
`trial-summary.json` and `ts-validation.json`. Validation used
`python3 <checkout>/userland/tools/ts_validation.py <trial>/T27.ts`;
validator SHA-256 was
`81d1d73804e0c6f6ee04994fd9d3f98f16b8ce64c6a29eab5042120fabcf4db5`.
Raw TS and card response contents remain local.

| Evidence file | SHA-256 |
| --- | --- |
| T27.ts | `af700d684fb6e6e567ef438a613b56341c5dd8134a601552ca00150c0bc5a765` |
| ts-validation.json | `6b599009bd64cc44fa4f0db9edfd86c071471efd4e293a2401327d86da094041` |
| trial-summary.json | `18b0f2e3cd0df6294d8fd5bbebe3a928fb7c4e31acd12abe5a177665a4bda9ea` |
| subprocess-status.json | `6a409d61c7079948649d176c27a2cc0f6acb284f03f640ce21fbd5da16673a34` |
| daemon.log | `a9ca0d4ad6bdb4bfa1d4f63dc38b005fb57f8702e69675001387881011ecdb15` |

### Candidate and acceptance boundary

The diagnostic build reproduced a stale loader claim/release failure; one
fixed-build cold trial completed cleanly. This is bounded evidence for the
repair, not a repeated cold-start reliability claim. It does not erase the
old 33-channel T27 failure or establish its cause.

After merging the repair, fix a new candidate SHA and verify two independent
same-source builds, then run the required exact-candidate regressions.
Branch artifact bytes must not silently become final-candidate evidence;
any identical-runtime inheritance needs an explicit comparison and bounded
scope. The eight-archive short tests, LNB measurements, card/USB physical
items, representative full-channel sweep and existing soak durations remain
subject to the unchanged acceptance procedure. No completed release sidecar,
formal release notes, tag or publication is asserted here.

## 2026-10-14 JST: fixed candidate b64577e and representative short regression

Recorded by dot. These results apply to the representative E03 machine only:
AnduinOS x86_64, PX-W3U3, primary/sibling USB paths `1-3.1` / `1-3.2`.
The test sequence began on 2026-10-13 JST and ended after midnight on
2026-10-14 JST. Times explicitly marked UTC below retain the execution clock.
Raw TS and card payloads remain local; this record publishes summaries only.

### Candidate and independent reproduction

- Source: `b64577e94389fe479d573eed57c745e2eb5573ef`
- Tree: `1f7ebf6a966656d747af7bf1a5feb619bcfffeb9`
- Candidate run: [38408329934](https://github.com/Khronos31/asicen-userland/actions/runs/38408329934), attempt 1, 20/20 jobs successful.
- Independent workflow-dispatch run: [38408518367](https://github.com/Khronos31/asicen-userland/actions/runs/38408518367), attempt 1, 20/20 jobs successful.
- Assembled candidate/reproduction artifact IDs: `11820352523` / `11821410522`.
- The nine distributed archives and `SHA256SUMS` were byte-identical across the two runs. Both manifests verified all nine archive hashes.
- Tested archive: `asicen-userland-0.1.0-linux-glibc-x86_64.tar.gz`,
  SHA-256 `a54f2e6f67a303fb4175dcaa6ee0d1dd0c31524fd45beabb024c939aa62298aa`.
- Tested Linux artifact: `11820665690`; artifact ZIP SHA-256
  `ed24e07da4a2a9cab5681cdf1df1e0d24d98cfb243265594ee833b73e9e768ca`.
- Local tested package:
  `/home/yunomin61/asicen-0.1.0/verified-artifact-38408329934-b64577e/package-glibc-x86_64`.
- Executable hashes:
  - `asicend`: `1fa0e9d3de1abfc019c453d41c0a0db1653282578d4b0bbe83339f93d92f1041`
  - `asicen-ts`: `e91ff275470b54fa3d6d75f9aab53eb714d6f5f188989f11b8916ec67f3a475e`
  - `asicenctl`: `35cac5d8c6d754344f1efca72ec91608aec5b4895e3e1dcc5e34b9842a7c0e3c`
- Loader SHA-256: `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`.
- PX4 reference remains v0.2.1 `dd1c1bea16a5871085d5f2a42c009364c1ec7345`.

Runner image versions matched between the two runs: Ubuntu x64
`20261004.327.1`, Ubuntu arm64 `20261004.142.1`, macOS arm64
`20260831.0302.1`, Windows 2022 `20261004.326.1`.
Compared effective inputs also matched: Alpine 3.22 index digest
`5291449c3df73caf6ed85e649dec1b9e818b39a5d8c871e97afc13e9cd5e8fa8`
and 61 package versions per architecture; Debian 11 digest
`6f519a81440354a85eb592c5f32109ab80605f6b892455983a6f618bf87fabe9`
with snapshot `20260825T000000Z`; Android NDK r27d / API 24 / Clang
18.0.4; macOS Xcode 15.4 / SDK 14.5 / Apple Clang 15.0.0;
Windows llvm-mingw 20250910-ucrt / Clang 21.1.1. This is not a complete
SBOM or verification of every tool executable hash. Windows remains
build-tested only and is not a 0.1.0 distributed binary.

The prior b41ea41b cold-start trial is retained in its own section. Comparing
that trial package to this fixed candidate found identical runtime binaries,
IFD, firmware and binary audit; four manifest/checksum/provenance files differed
with source identity. This bounded comparison does not certify the other seven
platform archives or substitute for their hardware tests.

### Representative full-channel regression: 33/33 passed

Executed 2026-10-13 14:14:36–14:22:12 UTC. Each channel requested 10 seconds,
one stream at a time. Terrestrial used receiver 1; satellite used receiver 0,
`--lnb-voltage 0`, the existing wall-fed installation, and no
`--allow-lnb-power`. No satellite-only LNB argument was passed to terrestrial.

| Channels | Count | Result |
|---|---:|---|
| T16, T17, T19, T21, T22, T23, T24, T25, T26, T27, T30, T31, T32 | 13 | PASS |
| BS01_0, BS03_0, BS05_0, BS09_0, BS13_0, BS15_0, BS19_0, BS21_0, BS23_0 | 9 | PASS |
| CS2, CS4, CS6, CS8, CS10, CS12, CS14, CS16, CS18, CS20, CS22 | 11 | PASS |

All 33 clients and both offline validators exited 0. All captures had zero
sync/invalid-packet, TEI, CC-discontinuity, duplicate and invalid PAT/PMT counts.
Daemon shutdown exited 0, without timeout or remaining processes; cached
release diagnostics reported both runtime functions released successfully.
Cleanliness here concerns saved TS and offline validators. It does not
establish the absence of losses before initial framing/alignment; raw USB and
framer discard counters were not independently captured for this sweep.
This does not erase the old f7352f4 T27 CC failure or establish its cause.

Evidence directory:
`/home/yunomin61/asicen-0.1.0/b64577e-e03-allch-20261013/`.
Summary SHA-256:
`17f593e35c88277eef96c8c04f7d54aec42205469e8e3e8818e028e279108635`.
Runner SHA-256:
`5b797113e0ff002f9e04062bbb4473a7b8cf09b38f3eb3940f27f35cc8f8d6bd`.
Shared validator SHA-256:
`81d1d73804e0c6f6ee04994fd9d3f98f16b8ce64c6a29eab5042120fabcf4db5`;
legacy validator SHA-256:
`de2fbab94b119975bc088e1f345c38f20574ffa2549e109cbb9599d556ed8902`.
The runner invocation was recorded; per-client argument descriptions were
reconstructed from the runner/configuration, not claimed as separately
captured argv arrays.

### LNB tester observations

The user disconnected the satellite coax from the wall-fed installation and
measured the device connector with its AC adapter attached.

- Explicit `--lnb-voltage 0`: user confirmed 0 V during the request.
- Explicit `--lnb-voltage 15`, daemon `--allow-lnb-power`: user confirmed 15 V.
- Both used receiver 0, `BS01_0`, `--tune-timeout-ms 30000`,
  `--duration-seconds 1`, output `/dev/null`. Without the antenna,
  client exit 5 was expected; daemon exit was 0 in both trials.
- After the 15 V request ended, the user observed a gradual voltage decline.
  The user then shorted the output to discharge it and reported 0 V.
  **Natural decay all the way to 0 V was not observed in this trial.**
  Do not record this as proof of an unassisted return to 0 V or of the
  discharge/protection circuit's implementation.
- The user reports that unassisted decay on this model takes over 90 seconds.
  A capacitive/high-impedance-meter explanation is a hypothesis, not a
  component-level finding from this trial. Shorting is not a prescribed
  validation step.
- The satellite coax was subsequently restored by the user. Later card/USB
  tests did not enable LNB power.

15 V evidence:
`/home/yunomin61/asicen-0.1.0/b645-lnb15-q2gxd7jw`.
The 0 V evidence is `/home/yunomin61/asicen-0.1.0/b645-lnb0-ocb8mjme`.
Request start times: 0 V at 2026-10-13 14:33:19.254138 UTC;
15 V at 14:39:21.019665 UTC.
Saved argv and exit records were checked. Before the actual runs, blocked
permission attempts did not reach device execution and are not extra trials.

### Card removal and return: passed

With one continuously running daemon, the user removed and reinserted the
same internal B-CAS card:

1. Present before removal; ATR retrieval exited 0, length 13 bytes.
2. Removed: `present=no`; ATR command exited 9, no ATR data.
3. Reinserted without daemon restart: `present=yes`; ATR retrieval exited
   0, length 13 bytes.
4. Harness reported `CARD_CYCLE_PASS`; daemon shutdown exited 0.

Commands were `asicenctl --runtime-dir R --instance card-cycle card-status`
and `card-atr` at each stage. The daemon used the two recorded USB paths,
the fixed package and its loader, without `--allow-lnb-power`.
Evidence:
`/home/yunomin61/asicen-0.1.0/b645-card-cycle-989g_yis`.
Raw ATR bytes remain local. This trial checked card presence/ATR recovery,
not new APDU application semantics or PC/SC/IFD compatibility.
The before/absent/restored status generation values were 1/2/3;
status snapshots reported initialized=no before each ATR request.
ATR observations were at 14:54:00.830643, 14:56:11.053131 and
14:57:29.940376 UTC on 2026-10-13.

### USB disconnect and fresh-daemon recovery: passed

- While T27 receiver 1 was receiving, the user removed the USB cable, leaving
  AC, antenna and card unchanged. The client reported `DISCONNECTED`,
  exited 7, and the daemon exited 7. There were no remaining ASICEN processes.
  A disconnect-related daemon exit 7 is recorded as such, not a normal
  shutdown exit 0. Local TS size was 281,673,632 bytes.
- After the user reconnected USB to the same port, the same candidate was
  started as a **new daemon**. T27 requested 10 seconds; client and daemon
  both exited 0, with no remaining processes.
- Recovery capture: 20,119,384 bytes / 107,018 aligned 188-byte packets,
  no trailing bytes; sync/invalid packets, TEI, CC discontinuities,
  duplicates and conflicts all 0. PAT 101/101 and PMT 232/232 sections valid.
  The 103,009 scrambled packets are not claimed as descrambled content.

Disconnect evidence:
`/home/yunomin61/asicen-0.1.0/b645-usb-unplug-y4s5upk0`.
Recovery evidence:
`/home/yunomin61/asicen-0.1.0/b645-usb-replug-5f0kbyi2`,
including `ts-validation.json` (SHA-256
`ded562cef99cce2e8a4f8b19fa52f4357ca15d9f27f45de3f02f4a2581740816`).
Disconnect capture was ready at 14:58:58.224606 UTC and the client ended at
15:01:17.183885 UTC. Recovery was ready at 15:03:47.616755 UTC and the client
ended at 15:03:57.662830 UTC on 2026-10-13. These harnesses save daemon/client argv and
exit records. The disconnect request used `--duration-seconds 360` with a
bounded physical-operation window; the recovery request used
`--duration-seconds 10`. No LNB permission or terrestrial LNB option was used.

### Remaining work and handoff

- This session owns the representative E03 regression only, not the other
  seven platform environments.
- The representative candidate's 2-hour soak has **not started**.
  At the user's direction, all soaks of 30 minutes or longer are grouped
  at the end, after short tests. This changes scheduling, not the required
  durations or matrix.
- LNB observations above remain qualified: natural decay to 0 V was not
  completed in this trial. Do not silently mark that observation as passed.
- Other platform hardware gates remain separate and are not certified here.
- The user deleted the old 2026-10-10 `/home/yunomin61/asicen-rc/soak/soak-t27.ts`
  after its 14,135,249,624-byte size/date matched the historical soak record.
  The old aggregate record is retained; that raw capture is no longer available
  for reanalysis and is not evidence for b64577e.
- This is a documentation-only results update, not release-note, tag, or
  publication approval.
