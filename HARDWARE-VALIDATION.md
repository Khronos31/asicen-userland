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
