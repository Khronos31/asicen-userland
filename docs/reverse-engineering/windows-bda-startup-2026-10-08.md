# Original W3U3 Windows BDA static review

Follow-up (23:07 JST): the Linux prerequisite left unresolved by this review
has now been identified and tested. Calling the public TF_bGetCusInfo before
Init populated the identity cache through a successful58-byte USB0c read;
both lanes returned Init=1. The guarded T27 trial still did not lock or
receive nonzero bulk data. See [the hardware record](../../HARDWARE-VALIDATION.md#customer_info-prerequisite-trial-2026-10-08-2307-jst)
and [the dedicated API audit](https://github.com/Khronos31/asicen-userland/blob/40343327409e38ba499032c5a54e044ac542bb0e/docs/reverse-engineering/official-init-customer-info-2026-10-08.md).
No GenEncSeed call was needed for that initialization result. The sections
below preserve the narrower findings of the preceding Windows review.

## Scope and provenance

This is a read-only static review of the original PX-W3U3 64-bit Vista/Win7/Win8 BDA driver, not V2 or PBDA. The examined SYS is `HDTV_PX_W3U3_BDA.sys`, SHA-256 `a032a28b28e5d239aa32b9c41e7d9f61c810ada4b120dcc214c58be770ea5b88`, size 185,728 bytes. It is PE32+ x86-64, image base `0x10000`; the image has no export symbols. RVA/file offsets match for the reviewed sections. Analysis used PE metadata, the exception function table, imports, and disassembly; the image was not executed.

The matching INF is `HDTV_PX_W3U_BDA.inf` in the same `BDA_driver_64(Vista_Win7_Win8)` directory. Its device models at lines 21–30 bind `USB\VID_0B06&PID_0005&MI_00` to the W3U3 install section and `VID_0B06&PID_0004&MI_00` to W3U2. This establishes that Windows PnP matches the W3U3 VID/PID before loading the bound function driver.

## USB/PnP code path observed

The `.text` function at RVA `0xac0–0xbcf` (image VA `0x10ac0`) is the device-descriptor fetch boundary. It allocates an 18-byte transfer buffer (`0x10b0a–0x10b15`), builds an URB with function `0x0b` (GET_DESCRIPTOR_FROM_DEVICE), descriptor type `1` (DEVICE), index 0, and transfer length `0x12` (`0x10b26–0x10b52`), then submits it through helper RVA `0x6ac` (`0x10b56`). On success, the returned descriptor-buffer pointer is stored at the Windows device extension `+0x28` (`0x10b69`), and the function then calls the configuration/interface path at RVA `0x768` (`0x10ba1`).

The helper at RVA `0x6ac` submits internal USB requests (`IOCTL_INTERNAL_USB_SUBMIT_URB`, code `0x220003`) through `IoBuildDeviceIoControlRequest` / `IofCallDriver` (`0x106fa`, `0x10725`). The configuration helper at RVA `0x768–0x87e` calls RVA `0x884` (`0x107f0`, `0x10877` call sites in image VA notation). The latter calls `USBD_CreateConfigurationRequest` at `0x108b3` and `USBD_ParseConfigurationDescriptorEx` at `0x108e1`.

The descriptor fetch stores a pointer to the 18-byte returned device descriptor; the standard descriptor layout places `idVendor` at byte offsets 8–9 and `idProduct` at offsets 10–11. The fetch path itself does not decode or compare those fields. The device-extension `+0x28` reference appears again in a teardown path that frees the saved descriptor buffer; I did not find an initialization use that copies VID/PID into a separate cache or compares it before tuner initialization. This is a bounded static finding, not proof that no such use exists elsewhere in the driver.

## Follow-up on candidate VID constant

The only `.text` occurrence of byte sequence `06 0b` reported at RVA `0x1e481` is not a VID/PID comparison. Within the function at RVA `0x1e1d4–0x1e66d`, the instruction stream decodes across it as `c1 ea 06` (`shr edx, 6`) followed by `0b d0` (`or edx, eax`). The enclosing function consumes eight input bytes and performs a table-driven bit transformation; its adjacent caller at RVA `0x1e674–0x1e6c3` invokes it as a helper. There is no comparison against `0x0b06` at this location, and I found no caller/dataflow connecting this helper to the saved USB descriptor buffer or the Linux extension offsets. This candidate is incidental machine-code bytes, not evidence for VID/PID preparation or a Windows equivalent of Linux `DTV_GenEncSeed`.

This bounded review did not recover a Windows initialization sequence equivalent to the Linux library's identity-cache preparation or establish a counterpart of `TF_DTV_GenEncSeed`. The BDA SYS is a separate kernel driver with a different device-extension layout; searching for identical structure offsets is not a valid equivalence test. Its verified descriptor-fetch path does not establish how the Linux library's four cached identity bytes should be populated.

## Comparison with the Linux `-2` result

On the Linux objects, `DTV_Init` (`DTV_Lib.o`, `.text+0x7250..0x72c5`) reads extension bytes `+0x476b..+0x476e` and returns `-2` if its VID/PID check fails. The observed USB descriptor and INF identify the actual device as VID `0x0b06`, PID `0x0005`; `TF_DTV_DevCreate` successfully opens the explicit node and runs `DTV_Start`, but that does not show that these `DTV_Init` extension bytes have been initialized.

`TF_DTV_Init` (`Transform.o`, `.text+0x330`) calls `DTV_Init` directly. `TF_DTV_GenEncSeed` is a separate exported call (`Transform.o`, `.text+0x250`); it is not called by that wrapper. Although Linux `DTV_GenEncSeed` references these extension offsets, this Windows review does not prove that it is the intended identity-population routine or that it is safe/required before raw capture. No seed/random-key operation, APDU, device operation, or attempt to bypass the VID/PID gate is recommended from this evidence.

## Conclusion and limitation

The Windows BDA package provides a useful comparison: Windows selects the original W3U3 driver by PnP hardware ID, fetches the device descriptor, and configures the USB interface. Linux USB core already enumerated the device, matched its descriptor, and bound the relevant interface before the harness opened its device node; this review does not claim those kernel steps are absent on Linux. The difference established here is narrower: the Windows function driver is handed a PnP device stack, whereas the Linux user-space library’s `DTV_Init` sees its own extension bytes, which were not proven initialized by `DevCreate`/`DTV_Start`. The candidate `06 0b` sequence in the Windows image is an incidental shift immediate/opcode, not a device-ID check. The exact Linux initializer prerequisite remains unresolved; this review is not evidence to invoke `TF_DTV_GenEncSeed` or to alter those fields manually.

The entire Windows tuner-initialization, frequency-setting and first-bulk call graph was not recovered by this review. The next narrow diagnostic is to observe the Linux extension's actual four identity bytes before `DTV_Init`, then trace their intended producer. The earlier return value -2 alone does not dynamically identify which failing branch was taken. No Windows or Linux vendor binary was executed, no receiver state was changed, and no LNB command was sent in this static review.
