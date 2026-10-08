# Guarded official-library Init(-2): missing customer-info preparation

Reviewer: **dot (OpenAI)**. Date: 2026-10-08.
Read-only investigation of the [guarded trial at d7998c8](https://github.com/Khronos31/asicen-userland/blob/d7998c84da7a6880fa6360f74b2b2e37b0fb87dc/HARDWARE-VALIDATION.md)
and its committed harness/USB trace. No hardware or vendor-code execution was performed here.
The original archive was retrieved again from research artifact 11511217050;
SHA-256 e1d68db2e09c584912a60363d89e56271ff3ad3b1bd0a319a7076fe914dad2c0 matches.
Offsets below are hexadecimal, member-relative .text addresses.

## Concrete missing preparation API

The SDK publicly declares TF_bGetCusInfo(deviceExtension, PCustomerInfo).
Its symbol is _Z14TF_bGetCusInfoPvP13Customer_Info.

- Transform.o:420..449 passes extension+4768 as the actual destination to
  bGetCusInfo, with fd from extension+4fc8.
- WDM_cmd.o:2b0..2c7 calls UsbDTV_u32GetCusInfo at210..2af.
  This sends vendor IN request0c, value0, index0, length58 (3a hex).
- The 58-byte Customer_Info contains VID at byte3..4 and PID at byte5..6.
  Thus the operation populates extension+476b..476e directly.
- Transform.o:44e requires bGetCusInfo return1 before copying the internal
  Customer_Info to the caller's output object.
- DTV_Lib.o:DTV_Init reads exactly those cached fields at728e..72bb and
  requires VID0b06 and PID0004/0005 at72bd..72f5.

The committed harness does not call this API; the guarded trace has no request0c.
DevCreate is not a substitute: DTV_Lib.o:fe0..102f opens, allocates and starts;
DTV_Device.o:179b zeroes the extension. The inspected startup path does not
establish this customer-info preparation.

A raw libusb customer-info read into an unrelated application buffer would not
populate this vendor extension. Use the real SDK type/API rather than guessing
opaque offsets or overwriting IDs to bypass the check.

GenEncSeed's VID/PID references at DTV_Lib.o:8dad..8dc6 pass pointers to
CalculateFinalKey; its3e8b/3e9b read those bytes. Those references do not establish
that seed generation prepares the identity fields. No seed API or guessed key
input is justified to solve this preparation omission.

## Why the early identity gate is strongly supported, not observed directly

DTV_Init has two -2 setters in the inspected function:
- 72c5: rejected VID/PID.
- 74f3: both TunerReset attempts failed, after calls7482 and74cc.

The latter route must first pass SysCtrlRead7408 and GPIO08-clear7445.
The committed trace contains DevCreate's startup SysCtrl read but no second
such read or frontend-init sequence before the wrapper's CF/bulk activity.
Together with the missing customer-info call and zero-initialized extension,
this strongly supports the early gate. Internal bytes/branch execution were
not logged dynamically, so it is not recorded as a directly observed branch.

## Why bulk traffic can appear after Init failed

Transform.o:365 calls DTV_Init;36f saves its return.
It then calls PollingThreadInit372 unconditionally. The sign test at377 tests
that later result, not the saved DTV_Init result. It can continue through
PID boundaries3af and StreamThreadRun3c0, then return the saved -2 at381.
The observed endpoint81 submissions therefore do not prove frontend init passed.

The signature names also matter: argument2 selects local lane, argument3
bMode==1 maps to internal mode2 (otherwise0), and argument4 is bAPKeyIdenfy.
It is not another lane selector. Preserve the existing bounded trial's arguments
when testing only the customer-info preparation difference.

## Minimum next diagnostic and limitations

Within a separately controlled trial, keep the existing LNB/GPIOEx exclusions.
After successful DevCreate and before the first Init, call TF_bGetCusInfo with
the real SDK Customer_Info type. Record:
1. API return and returned VID/PID;
2. the request0c USB completion status and actual length58;
3. ideally the four cached ID bytes immediately before Init, read-only;
4. the subsequent Init return and whether its own SysCtrl/frontend calls occur.

Do not continue after invalid/failed identity data. Do not patch the cached IDs,
bypass the gate, or add seed generation. Passing this gate would resolve only
this harness prerequisite, not the previous locked endpoint82 zero-byte problem.

The vendor API success flag alone is insufficient: WDM_cmd copies its stack
buffer even on an ioctl failure. In the recovered kernel,
fdmx_irq1f04..1f21 only signals completion; the control path waits at1ea4 and
returns success at1ecc without validating URB status/actual_length.
usbmgr_ioctl1057..1068 copies the requested length. Trace validation is essential.
The wait_for_completion is unbounded; do not describe the vendor read as a
proven finite-time operation merely because a TimeOUT field exists.
Keep an external trial bound and explicit cleanup strategy, accounting for
failure paths. No diagnostic or hardware operation was run by this review.
