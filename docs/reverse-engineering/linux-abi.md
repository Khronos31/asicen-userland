# Recovered ASICEN Linux ABI

This document records factual ABI/protocol observations recovered from the historical PLEX Linux package. It intentionally avoids copying vendor implementation code.

## Device IDs

| Model | Runtime USB ID | Receivers | Notes |
|---|---|---:|---|
| PX-S3U | `0b06:0001` | 1 | combined T/S frontend |
| PX-S3U2 | `0b06:0003` | 2 | T + S |
| PX-W3U2 | `0b06:0004` | 4 | official package reuses W3U3 userspace library |
| PX-W3U3 | `0b06:0005` | 4 | primary bring-up target |
| PX-W3U3 V2 | `0b06:0006` | 4 | recovered from official Windows INF |

Firmware-loader IDs observed in official drivers: `1738:5211` and `1738:5216`.

## Kernel/userspace ioctl ABI

The official userspace library uses five raw ioctl numbers:

| ioctl | Recovered role |
|---:|---|
| `0x100` | generic USB vendor control transfer |
| `0x101` | bulk stream start/stop control |
| `0x102` | query buffered stream length |
| `0x103` | stream auxiliary/reset operation; exact public semantic still to confirm |
| `0x104` | copy buffered stream data to userspace |

## Vendor control transfer

`ioctl(0x100)` is a thin wrapper around a USB vendor control transfer.

Recovered mapping:

- direction 0 -> `bmRequestType = 0x40` (vendor, host-to-device)
- direction 1 -> `bmRequestType = 0xc0` (vendor, device-to-host)
- command byte 0 -> `bRequest`
- command bytes 1..2 -> little-endian `wValue`
- command bytes 3..4 -> little-endian `wIndex`
- `CxLen` -> `wLength`

The kernel-side DWARF type `_AUSBDTV_USB_CONTROL_TRANSFER_STRUCTURE` is 24 bytes on the recovered x86_64 build and contains tuner number, five command bytes, data pointer, length, direction and timeout.

## Kernel context

`_AS11USBDTV_CONTEXT` is 2296 bytes on the recovered x86_64 build. Important observations:

- two bulk URB banks: `bulk_urb[2][128]`
- two stream objects: `StreamObject[2]`
- two stream-started flags
- two tuner-init flags
- two tuner pointers

This strongly indicates that one runtime ASICEN USB function owns two tuner/stream lanes. W3U3/W3U2 then combine multiple such USB functions at enclosure level.

## Stream object

`_AUSBDTV_STREAM_OBJECT` is 1168 bytes on x86_64 and contains one pipe number, 128 transfer objects, ring-buffer state, ISR buffer state, flow rate and stream-started state.

## High-value recovered symbols

Kernel module:

- `AUSBDTV_SendUSBControlTransfer`
- `AUSBDTV_StartBulkStream`
- `AUSBDTV_StopBulkStream`
- `DTV_SendUSBControlTransfer`
- `DTV_StreamDataRead`
- `usbmgr_ioctl`

W3U3 userspace library:

- `USB_I2C_Read` / `USB_I2C_Write`
- `USB_BulkStreamStart` / `USB_BulkStreamRead` / `USB_BulkStreamStop`
- `DTV_SetTunerFreq` / `DTV_SetTunerTSID`
- `DTV_GetTunerSignalLevel`
- `bBCardInit` / `bReadBCAS_Data` / `bWtBCAS_Data`
- `DTV_DecrypTS` / `DTV_DecrypMultiTS`

## Known control requests already visible in the userspace binary

These are observed request bytes from command construction and should be cross-checked on hardware before being treated as a stable public API:

- `0x0a`: query device high-speed state
- `0x0c`: customer info read
- `0x17`: system-control read
- `0x18`: system-control write
- `0x19`: I2C read variant
- `0x1a`: device random-key read

More request IDs remain to be extracted from `WDM_cmd.o`/`FUSBDTV.o`.

## Provenance

Primary binary evidence: PLEX `PX-SERIES_ver.1.0_Linux_Driver.zip`, recovered from the vendor URL on 2026-10-08. The x86_64 kernel module contains DWARF debug information and is not stripped.
