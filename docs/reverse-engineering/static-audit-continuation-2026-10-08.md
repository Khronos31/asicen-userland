# Static audit continuation after the zero-byte branch comparison

Reviewer: **dot (OpenAI)**. Date: 2026-10-08.
Remote checkpoint, verified through GitHub: `2bfa6516a242d497b2a8f373b68918a9a07b245b`,
on `dot/static-audit-20261008`. Its diff adds hardware observations, not source
changes. Source inspection used the local working files prepared and published
as 948b890 in the previous audit; the GitHub commit diff confirms no source changes in 2bfa651.
Object/firmware identities and addressing follow [the first audit](static-audit-2026-10-08.md).
No hardware, vendor-code execution, module loading, or change to acquisition defaults
was performed for this continuation.

Unless explicitly stated otherwise, register addresses, object offsets, firmware
addresses and USB setup values below are hexadecimal. Sizes, counts and elapsed
intervals are decimal.

## Result

The user verified all 18 tests and compared main with the three ordering fixes.
Both locked at A9 and produced zero bytes in normal, stopping, and cancel/drain
phases. Settings were restored and read back. The tests pass, but the hardware
result does not demonstrate a TS-output fix. The fixes were tested together,
not as three independent causal experiments.

The continued inspection has not established another source defect that explains
the zero-byte result. It narrows several tempting but unsupported modifications.
The most informative next evidence is an official-driver trace from enumeration
and initialization through the first nonzero bulk completion.

## GPIOEx is a real omitted operation, but not necessarily a different state

- `DTV_Device.o:DTV_Start` calls `TC_PowerTunerDemod(local 0,off)` at `.text 1742`,
  and local 1 at `1758`.
- `TunerControl.o` returns for nonzero local (`28e9`); power=0 branches at `28f2`
  to `2a08`. The local 0 off path includes GPIOEx value 1/mask 1 at `2a21`.
- Its real transport chain is `TunerLib.o:267` -> `FUSBDTV.o:755` ->
  `WDM_cmd.o:630`. The final request is vendor IN `10`, wValue `0101`,
  wIndex0, length 1. Current `plan_startup_subset` and `plan_safe_power_on`
  in `userland/src/frontend_sequence.cpp` do not emit this request.
- Firmware `2738..2770` masks the high wValue byte with03 and updates P3.0/P3.1
  from the low byte. It does not directly write the DSC or CF registers.
- Firmware startup already sets both P3 output latches high (`48a0`, `48a2`). Request13's
  serializer (`230c` -> `57bd`) can leave the P3.0 latch low. The suspend/resume
  save/restore path is conditional on c489 bit 2 and includes temporary high
  latch states (`5425..5447`, `51e6..520b`). Therefore omitting GPIOEx 01/01 does not establish a mismatch
  after a clean boot with no such earlier operation.
- GPIOEx read11 (`2778..279a`) returns those two pin bits. Ordinary GPIO 76 does
  not report them. Wire-level read: vendor IN c0, request 11, value 0, index 0,
  length 1; the returned byte is the pin bitmap, not an I2C-style status prefix.

Physical function and board wiring of these pins remain unproven. Do not replay
the entire off sequence: it also clears GPIO 20, which has LNB-power consequences.
The minimum next diagnostic is a GPIOEx readback comparison, without a new
write. A read reports pin levels, not necessarily output-latch contents. Even a
mask 1 command samples P3 and writes both low-bit latches, so exact preservation
of the other latch is not guaranteed by that mask alone. Do not prescribe a
blind GPIOEx write until the official trace and board behavior justify it.

## Initialization names do not prove missing USB traffic

For this recovered Linux library:

- `DTV_Init` calls Get_DevRandomKey at `DTV_Lib.o:7517`, but
  `FUSBDTV.o:40..45` immediately returns `c0000001`, with no I/O.
- `bBCardInit` can call Set_RandomKey at `DTV_Lib.o:701a`, but
  `FUSBDTV.o:b0..b5` is another immediate failure-return stub.
- `DTV_Reset_EncChipEx` returns success without I/O for revision 11
  (`DTV_Lib.o:4844..485d`). Its revision 16 path calls SysCtrlWrite, itself
  an immediate failure-return stub at `FUSBDTV.o:100..105`.
- For local 1, `bBCardInit` first reads controller4a/09 (`70e8..710d`). A
  successful type 0f/10 result bypasses the GPIO80 reset (`7188..71a3`).
  This is a controller-type check, not proof of card presence or successful APDU.

These facts do not justify adding random-key, encryption-reset or card commands
as presumed prerequisites for the first raw byte. Nor do they prove that every
hardware-specific controller operation is unnecessary.

## USB endpoint setup does not show a missing alternate setting

- Firmware descriptors at code `54bb` (high speed) and `54fd` (full speed)
  expose interface 0, alternate 0, bulk IN81/82, with maximum packets512/64 bytes
  respectively. They do not expose an alternative streaming setting to select.
- `as11usbdtv.ko:usbmgr_probe` (`880..e6b`) and `usbmgr_open` (`53c..75b`)
  do not add an explicit setup transfer. The vendor-control builder at
  `1d62..1d6f` only emits request types40/c0, not standard SET_INTERFACE or
  CLEAR_FEATURE requests. No explicit driver clear-halt/reset/alt-setting
  operation was found on this path.
- Firmware SET_CONFIGURATION1 performs endpoint setup at `55cd/55d0` (HS)
  or `55ed/55f0` (FS). SET_INTERFACE(interface 0,alt 0) also performs endpoint setup
  (`5987..599f`), so it is not necessarily a harmless no-op; however its
  absence is not established as a userland defect.
- Current `asicen_frontend.cpp:1296` calls `claim_interface(0)` directly; that
  wrapper calls libusb_claim_interface, without an alternate-setting change.
  The separate `claim_endpoint` helper is not the frontend's selected path.
  Neither blind SET_INTERFACE nor clear-halt is justified as a fix.
- A significant firmware quirk: SET_INTERFACE(interface 1,alt 0) calls the same
  global clear at `5987` but does not restore the interface 0 bulk mapping,
  because the setup guard requires selected interface c502==0 (`58d9`, `5920`).
  Routine5754 clears8040..43,8060..63,8030..31 and endpoint mapping banks
  8080..84/8090..94. This is proven firmware behavior, not proof that it has
  happened in the user's run. The official kernel probe rejects the
  one-endpoint interface (`91f..928`). Do not touch interface 1 experimentally.
- Four 4096-byte transfers and host submission before DSC match a recovered
  official path. Host ring capacity is a separate parameter, not a count of
  128 in-flight transfers. This does not establish that every official call
  selects4096 rather than65536; retain actual trace sizes when comparing.

## A post-lock branch exists; Linux reachability remains unproven

The previously discussed `DTV_PollingThread` recovery sequence is conditional,
not an unconditional TS-enable step immediately following every lock.
`DTV_Lib.o:9777..9788` first requires a non-null per-lane stream-object pointer
at device+440; `97a4..97ac` also requires its active flag at device+438.
Further queue/state and lock checks precede the controller/filter operations.

The recovered Linux `FUSBDTV_StartBulkStream` (`FUSBDTV.o:390..3bc`) forwards
only fd, lane and numeric sizes into the kernel ioctl wrapper; it does not
populate those userspace stream-object fields. This audit found no direct
stores establishing them in the inspected archive, but that negative search
alone does not prove all possible indirect or external initialization absent.

Consequently, the recovery body is proven code, while its execution in this
Linux configuration is not proven. Look for its real controller/reset/CF
transactions in the official trace instead of assuming its presence or adding
more repetitions to match a potentially unreachable branch.

## Remaining real tuner polling and optional routing

`TC_PollingThreadDelayTime` (`TunerControl.o:1b0`) requests 300 ms; the shared
loop tests the selected local-active flag before `Tnim_PollingThread`
(`DTV_Lib.o:94a6..94b4`). `TC_PollingThread:1958` invokes Fiti_LAN_Gain.
It is source 0 only (`13bb`) and not guarded by a demod-lock check.
Its real operations are FC0012 registers12/13/0d/10, including read-dependent
state transitions. Their wire transport uses the demod FE/C6/C7 tuner tunnel.
No separate demod-configuration, controller, CF, DSC or GPIO update was found
in this routine.
Current userland omits this periodic tuner adaptation. RF/TS impact remains
unverified; replacing it with a guessed fixed write is not equivalent.

The optional D2 initialization checks probe success, whereas later serial-mode
recovery toggles require both probe success and returned version1
(`DTV_Lib.o:689f,69a6,98c3,a984`). A failed D2 probe does not meet those guards.
No D2 command was added based on a function name or an assumed chip presence.

## Official Linux environment identity

The provided package's Read me names CentOS 6.3. Both `loader.ko` and
`as11usbdtv.ko` have `.modinfo` vermagic:

`2.6.32-279.el6.x86_64 SMP mod_unload modversions`

This is the exact ABI target in these artifacts, not a promise of compatibility
with another kernel. A generic contemporary Linux install alone is not equivalent.
Do not force-load an incompatible module to obtain the trace. Keep an old test
system isolated from unrelated data and network services.

## Minimum diagnostic without changing stream configuration

Retain the existing capture settings and first collect read-only evidence:

- Actual negotiated speed, active configuration and interface 0 alternate setting.
- Endpoint 0x82 GET_STATUS: bmRequestType=0x82, bRequest=0x00,
  wValue=0x0000, wIndex=0x0082,
  length 2. Record the transfer result and both reply bytes, not just a boolean.
  Endpoint 81 is an optional peer comparison. Firmware `506a..5115` maps
  hardware bit 3 of8041/8043 to bit 0 of reply byte 0, with reply byte 1 zero,
  after validating its endpoint map; a failure is also
  relevant evidence and should not be converted into a clear-halt action.
- GPIOEx read11 as described above, preferably before and after initialization.
- Existing queue/DSC/completion diagnostics, retaining every actual_length.

These are observations, not a claimed fix. A later one-factor mutation should
be selected from an actual differing official transaction, rather than bundling
GPIOEx, endpoint reset, timer and tuner changes. Do not clear a halt speculatively.

## Trace collection boundary and comparison order

Capture before driver loading / power-on and retain both USB functions through
runtime re-enumeration. Track physical port paths because bus/device numbers can
change. Keep command timestamps, setup fields, response data/status and bulk
requested/actual lengths, not just application success messages.

1. Enumeration, firmware identity, SET_CONFIGURATION and SET_INTERFACE.
2. GPIO and GPIOEx, shared frontend initialization, initial773143 kHz tune and
   the requested T27 retune; controller/D2 probe replies and actual branches.
3. CF reads/writes, reset09, DSC06 and first bulk submission/completion.
4. Any genuine post-lock recovery transaction, with its preconditions and timing.
5. The first nonzero endpoint 82 completion, separately from decoded TS validity.

If the official driver also produces zero bytes, first distinguish USB setup,
frontend lock and actual application data; it is not a successful reference trace.
If official reception succeeds, compare its earliest divergent real transaction
before proposing a one-factor replay. Preserve full traces privately; unrelated
USB traffic and any card/cryptographic data need not be published in an issue.
