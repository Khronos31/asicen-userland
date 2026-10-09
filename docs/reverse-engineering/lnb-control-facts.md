# LNB control: five ASICEN USB models

## Scope and conclusion

Static review on 2026-10-09. No vendor executable/driver was run, no hardware was
accessed, and no electrical voltage was measured. The operations below are
protocol facts rather than physical acceptance results. Proprietary binaries and
disassembly dumps are not included in the repository.

Physical LNB power support and a working software-controlled switch are separate
claims. Product documentation establishes physical power support for all five
models; the reviewed software establishes the following narrower result:

| Model | Proven software selector | ON | OFF | Scope |
| --- | --- | --- | --- | --- |
| PX-S3U | None recovered; official setter is a no-op | Unknown | Unknown | External DC15V supply is documented; its electrical switching topology is unproven |
| PX-S3U2 | None recovered; official setter is a no-op | Unknown | Unknown | Bundled LNB registry utilities incorrectly target W3U2 |
| PX-W3U2 | GPIO request08, mask20 | value00 | value20 | Shared primary function |
| PX-W3U3 | GPIO request08, mask20 | value00 | value20 | Shared primary function |
| PX-W3U3 V2 | GPIO request08, mask20 | value20 | value00 | Shared primary function, with GPIO80-dependent cutoff |

**V2 has the opposite polarity from original W3U3.** No independent 11V/15V
voltage selector was recovered. Do not infer one from the numerical voltage
values accepted by a compatibility API. No GPIOEx bit is established here as an
independent LNB selector.

For S3U/S3U2, neither a successful no-op nor a cached desired value establishes
ON or OFF. An implementation must return a clear unsupported-control result
rather than claim it switched the antenna supply. This does not negate either
model's documented physical power capability. It also does not prove their
supplies are always-on/pass-through: that requires a schematic or measurement.

## Reproducible primary inputs

Official downloads inspected:

- [PLEX Linux 1.0](https://plex-net.co.jp/plex/PX-SERIES_ver.1.0_Linux_Driver.zip)
- [S3U Windows 1.0.4](https://plex-net.co.jp/plex/px-s3u/PX-S3U_Ver.1.0.4.zip)
- [S3U2 Windows 1.0.5](https://plex-net.co.jp/plex/px-s3u2/PX-S3U2_ver.1.0.5.zip)
- [W3U2 Windows 1.0.3](https://plex-net.co.jp/plex/px-w3u2/PX-W3U2.Driver_Utility_Package_Ver.1.0.3.zip)
- [W3U3 V2 Windows 1.0](https://plex-net.co.jp/plex/px-w3u3v2/Driver_PX-W3U3_V2_Ver1.0.zip)

The Linux archive SHA256 is
`11a84eaef0157ac08c0b4128aa622a625e8914a28c59ef06e76378ee6094c5de`.
Its `PX-SERIES_ver.1.0_64bit_Linux.gz` contains the
`ReleaseToCustomer_64bit_130109_2` directory. Library hashes:

| Library | SHA256 |
| --- | --- |
| libPlexLib_S3U.a | 30d9311acfc254e00f52b62a2bdfee50046d130837b939290b0252da8b2ce4c1 |
| libPlexLib_S3U2.a | e75894fec8180014bf1c77c2c847374f56c53ebc52cf87fa85ea7bdad167ebc3 |
| libPlexLib_W3U3.a | e1d68db2e09c584912a60363d89e56271ff3ad3b1bd0a319a7076fe914dad2c0 |

Windows hashes:

| Image | SHA256 |
| --- | --- |
| S3U BDA x64 | ff705b65919dd8f11a70e843f586524973aeef16f1a4f3b65d1ca56b4cf3beae |
| S3U PBDA x64 | 22f36876a2dfc66a566c3f1772965b6f72d9a7d8a289ffdb8baf9013bde25082 |
| S3U BDA x86 | d9e79530611f636a2c3ba912cdc7dbe8c62e60068d78ba77ebafd8ab9308df2e |
| S3U PBDA x86 | 8c70d5246a077e79e4365fcfea8ef774caa47dc7501db16925ddec268b452ec2 |
| S3U2 BDA x64 | 900c4d55245d1550575364eeeaeeaeec33f04ddc3e8a783059eaa5eab92d2450 |
| S3U2 PBDA x64 | 1e5f1c0844fd09085c4aa56c8971cbf93f90a29d57ffee91c0401c85b7ee747e |
| S3U2 BDA x86 | 357e4d8c6fac136358469b5329cc80ba3be61e3eaeea603ff5b739d34de465fe |
| W3U2/W3U3 BDA x64 in V2 package | a032a28b28e5d239aa32b9c41e7d9f61c810ada4b120dcc214c58be770ea5b88 |
| V2 BDA x64 | 5c7174d62eef7d704f44904ac1336261135a1edfccd2776e5e1c45ce7088f0f2 |

Linux locations below are object `.text` offsets. Windows locations are **virtual
addresses**, with image base `0x10000`; subtract `0x10000` for the corresponding
RVA/file offset in the reviewed images. This differs from the RVA convention in
[v2-frontend-facts-2026-10-09.md](v2-frontend-facts-2026-10-09.md).

## W3U2 and original W3U3

Linux `TunerControl.o:TC_SetLNB` is at `0x2d0`. It accepts selector0 only. Its
third argument equal to1 selects `TLIB_SetGPIO(extension, 0x00, 0x20)`; all other
values select `(extension, 0x20, 0x20)`. A nonzero selector returns0 without a
transfer. The GPIO call result is ignored and the successful-selector path
returns1; a new implementation should check its own transport result instead.

The complete value/mask forwarding chain is:

1. `TC_SetLNB`, calls at `0x2ea` (OFF) and `0x307` (ON).
2. `TunerLib.o:TLIB_SetGPIO`, `0x290`, forwards both byte arguments unchanged.
3. `FUSBDTV.o:FUSBDTV_Cmd_Set_GPIO`, `0x710`, calls `ucSetGPIO` at `0x725`.
4. `WDM_cmd.o:ucSetGPIO`, `0x6d0`, calls `UsbDTV_u32GPIO` at `0x6f0`.
5. `UsbDTV_u32GPIO`, `0x640`, emits command bytes `08,value,mask,00,00`, one-byte
   vendor-IN response. Thus `bmRequestType=c0`, request08,
   `wValue=(mask<<8)|value`, `wIndex=0`, length1.

The only Linux `TC_SetLNB` relocation in `DTV_Lib.o` is the polling-thread call
at `0x920c`, passing selector0 and state0. Windows supplies the dynamic policy:
original x64 BDA registry read at VA`0x1e913` is followed by active-low GPIO
selection at `0x1e918..0x1e92f`. A separate loop at `0x209ce..0x20a06` reads
`LNBONOFF` and applies the same mask/polarity when support-feature bit80 is clear.
The GPIO helper at `0x116c4` constructs request08 with unmodified value/mask.

W3U2 and W3U3 BDA x64 images bundled in the V2 package are byte-identical. The
Linux `ShellScript_Lib.sh` explicitly selects the W3U3 library makefile and demo
for either device4 (W3U2) or5 (W3U3). These are independent source grounds for
sharing this control profile while retaining model/PID and topology checks.

### Lifecycle interference

`TC_PowerTunerDemod` at `0x28e0` is a separate shared-board operation. Its ON
branch writes GPIO20 high at `0x29a6`, matching LNB OFF. Its board-OFF branch at
`0x2a08` clears GPIO04, sets GPIOEx01, then clears GPIO20 at `0x2a30`, which has
the LNB-ON polarity. Do not blindly reuse that final GPIO20 operation for an
LNB-managed shutdown. Board cleanup and explicit LNB policy must be coordinated,
and unrelated GPIO snapshot restoration must exclude the managed LNB bit.

## V2 polarity and feedback cutoff

V2 x64 BDA's main polling thread begins at VA`0x1f59c`. It resolves the shared
owner from extension+`0x5c0`. At `0x1f676` it reads `LNBONOFF`; the comparison at
`0x1f67b` sets GPIO20 **high** for1 and low otherwise, through `0x118c0`.
The explicit property setter at `0x1a397..0x1a3a8` uses the same polarity.

The independent grouping/polling thread has a second, more complete path:

- `0x20904`: only `(support_feature & 0xc0) == 0` applies the selector on that
  function, identifying the primary role.
- `0x20923`: reads registry desired state; `0x20928..0x2093c` writes value20 for1
  or value00 otherwise, always mask20.
- `0x20941..0x20946`: waits20ms after applying this setting.
- `0x20951..0x2096d`: when primary and desired state is1, reads GPIO.
- `0x20972..0x20988`: if GPIO20 is set **and GPIO80 is clear**, immediately
  clears GPIO20.
- `0x2098d..0x209a0`: changes the remembered/registry request to0 after cutoff.
- `0x20dbc..0x20dcd`: waits100ms per loop. The feedback check runs each loop
  while requested ON; the registry reapply occurs every20 iterations.

GPIO80's exact electrical meaning is not named by this evidence. It is a
source-proven feedback/fault predicate, not a measured overcurrent threshold.
An implementation should preserve this cutoff behavior, propagate read failures,
and must not report an accepted ON setting if the feedback condition fails.
A one-time successful GPIO write is insufficient to replicate the source's
ongoing behavior.

The V2 write helper `0x118c0` emits request08. The read helper `0x1194c` emits
request0f with zero value/index and a one-byte vendor-IN response; on success it
copies the response directly to caller and cached GPIO state. There is no I2C
status byte. The common transfer helper `0x111b8` constructs a vendor-device
URB (function17), transfer flags1 (IN), one-byte length from `r9w`, request from
command[0], value from command[1..2], and index from command[3..4].
Masked digital readback can check control levels, but does not
measure connector voltage, current, or polarity.

### V2 lifecycle interference

Revision11 startup writes GPIO value`a7`, mask`fb` at `0x14bbe`. This selects
bit20 high and therefore conflicts with a default-OFF LNB policy unless the
managed bit is excluded or explicitly forced low. Similar broad-mask writes
occur in lifecycle routines; a LNB owner must isolate its bit consistently.

The shared-board OFF routine at `0x2fdbd..0x2fe1c` sets GPIO40, waits10ms, sets
GPIO08 and waits100ms. It does not itself clear GPIO20. Explicit LNB cleanup is
therefore necessary; do not assume board shutdown automatically establishes
antenna power OFF.

## S3U: software setter is a no-op

Linux `TunerControl.o:TC_SetLNB` at `0x270` consists of `return 1`, with no GPIO,
GPIOEx, I2C, or other transfer. `DTV_Lib.o` still calls it at `0x9175`.

Windows corroboration is unusually strong:

- x64 BDA main polling reads `LNBONOFF` at VA`0x1e541` into `[rsp+0x84]`; the
  containing routine never consumes that value.
- Its second thread reads at `0x200bd` into `[rsp+0x30]` and discards it.
- x64 PBDA main polling reads at `0x22f1a` into `[rsp+0x90]`; its second thread
  reads at `0x24905` into `[rsp+0x30]`. Both values are discarded.
- x64 BDA property ID44 setter at `0x191d4..0x191ea` writes the low input byte
  as a registry DWORD through `0x1de9c`; no hardware setter remains. PBDA's
  equivalent is `0x1ae3e..0x1ae54`, registry writer `0x2287c`.
- x86 BDA retains the three hardware-setter calls: `0x162ea`, `0x1a8fd` and
  `0x1f9e1`, all targeting `0x22750`. That target is literally
  `mov al,1; ret 16`, a four-argument successful no-op.
- x86 PBDA independently retains calls at `0x17e54`, `0x1e277`, `0x23742` to
  the same no-op at `0x260e6`.

This accounts for the apparent x64 omission: the empty setter was optimized
away. Generic raw-GPIO properties do exist, but their existence does not assign
an LNB meaning to a pin.

S3U GPIO20 is exercised in `TC_MOS_POWER` and `TC_PowerTunerDemod`, as part of
board startup/reset. Linux board-power path `0x39f..0x3d0` clears20, waits100ms,
sets20, waits100ms, following GPIO44 pulses. Windows x64 equivalent at
`0x25d4c` calls its GPIO helper with value00/mask20 at `0x25dc2`, then
value20/mask20 at `0x25dda`. These are not linked to an LNB request and cannot be
relabelled as an independent antenna-power switch.

## S3U2: same no-op, misleading bundled registry files

Linux `TunerControl.o:TC_SetLNB` at `0x280` is also an immediate successful no-op;
its `DTV_Lib.o` call is at `0x91f5`.

- Windows x64 BDA reads and discards `LNBONOFF` at `0x1e0f6` (`[rsp+0x84]`) and
  `0x1fc8d` (`[rsp+0x30]`).
- Windows x64 PBDA reads and discards it at `0x22ada` (`[rsp+0x88]`) and
  `0x24429` (`[rsp+0x30]`).
- Windows x86 BDA retains the setter at `0x2270e`, literally
  `mov al,1; ret 16`; callers are property setter `0x1632c`, periodic loop
  `0x1a9c5`, and main polling `0x1fafb`.

The official 1.0.5 archive includes a folder named `Utility/S3U2.LNB.reg`, but
both contained files are named `LNB_ON_PX_W3U2.reg` /
`LNB_OFF_PX_W3U2.reg`. Their contents really target
`HKLM\SOFTWARE\ASICEN\LNBPower\0B06_0004`, set `DeviceName=PX-W3U2`, and change
`LNBONOFF` to1/0. S3U2's runtime PID is0003. Neither the folder name nor those
files prove S3U2 software control.

The S3U2 board startup sequence changes GPIO20 and GPIOEx01/02, but does not
use an LNB desired argument. `TC_PowerTunerDemod` at `0x300` only tests the local
source and shared power argument. Its ON branch pulses GPIO20 at
`0x3f8/0x414/0x42d` (low/high/low with10ms waits), clears GPIOEx01 at `0x497`,
and clears GPIOEx02 at `0x54f`. The OFF branch sets GPIO20 at `0x365`, sets
GPIOEx02 at `0x344`, and sets GPIOEx01 at `0x377`. These facts belong to the
board power/reset profile; they do not establish a separable LNB selector.

## Implementation requirements derived from evidence

- Keep model-specific polarity and primary/shared ownership explicit.
- Preserve strict errors and digital verification instead of copying vendor
  wrappers that return success unconditionally.
- Keep the managed GPIO bit out of unrelated snapshot restore and broad-mask
  startup/shutdown writes. Apply explicit desired OFF before releasing ownership.
- Preserve V2 feedback cutoff while ON, with bounded serialized USB operations.
- Report no software-control capability for S3U/S3U2 until another authoritative
  source or a measurement establishes the actual control path.
- Keep a physical voltage/tester check outstanding for every model; static and
  synthetic checks must never be described as electrical validation.
