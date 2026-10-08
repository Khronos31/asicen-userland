# PX-W3U3 first hardware validation

## Scope

Validate the research skeleton against the attached PX-W3U3, starting from
research commit `7f4ff0e77e5570a9ac810f5742933c7135fdbc53`.
The local userland checkout retains the research history. Its intended origin
is `Khronos31/asicen-userland`. The GitHub repository was created private on
2026-10-08; public visibility is deferred to the 0.1.0 release per user
instruction. No release has been made.

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
DSC start replied1 and DSC stop succeeded. No USB/output errors were reported;
zero data was correctly treated as failure. Device filter/output start remains
unimplemented and is the next gate. No MPEG-TS or B-CAS success is claimed.
Log: `evidence/raw/capture-primary-T27.txt`; sample is an empty local binary.

### CF experiment and controller revision

Primary lane1 CF control read (`c0:04 value00c0 length2`) returned `01 04`.
Parent set PID boundaries41/43 to `1fff`, read both back, sent channel reset
`c0:09 value0001 length1`, and wrote CF40=`0b` (lane1 subcommandc0). Control
readback was `01 0b`. A subsequent 5-second capture still returned zero bytes
and exit1, with DSC stop successful. CF40 was restored to04 and read back as
`01 04`; this restores that register only, not the entire device state.
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

### Raw-read prerequisites and DSC state observation

Static inspection of the original open/init/stream path found no call to
`DTV_GenEncSeed` before the first raw USB read. The normal link-data software
transform follows that read. This does not establish the ASIC's internal
requirements or prove that returned bytes will already be ordinary TS.

Parent compared primary lane1 CF40..44 before DSC start, while started, and
after DSC stop. All three reads returned `01 04 00 20 1f ff` (first byte is
the wrapper reply). Start request07 and stop request06 each returned01;
cleanup issued stop again before releasing interface0. The final terrestrial
lock read returned `01 a9`. This measurement shows no change in those five CF
bytes during DSC; their relationship to stream production remains unverified.

### Optional CF40 filter-start capture diagnostic

`asicen_frontend --filter-start --queue-depth 4 capture` is an opt-in,
local-1-only diagnostic. It snapshots CF40 before stream setup, applies the
source-observed selector-on bits before queue/DSC start, then reads CF40 again
and sets the source-observed filter-start bit after DSC succeeds. It restores
the original CF40 byte after stopping DSC and draining queued transfers on all
capture outcomes. The option is disabled by default.

Parent's5-second primary T27 trial returned zero bytes/exit1. A second trial
combined the same post-start filter operation with controller05 candidate20;
it also returned zero bytes/exit1. Logs:
`evidence/raw/capture-primary-T27-post-start-filter.txt` and
`evidence/raw/capture-primary-T27-post-start-filter-controller20.txt`.
After the first trial CF40 read back04, confirming the diagnostic restored
that byte; PID boundaries still reflected ordinary stream setup. Parent
restored all five saved CF bytes after each trial. Final readbacks were
CF40..44=`04 00 20 1f ff`, controller05=00, GPIO76 and terrestrial b0=A9.
The source-observed filter-start order alone or combined with candidate20
does not resolve zero-byte acquisition. Local build and17 CTests passed,
including new ordering, restoration-failure and CF-response semantics cases.
No satellite tuning, LNB enablement or card APDU occurred.
