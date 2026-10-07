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
| `0x103` | clean/reset a tuner stream buffer |
| `0x104` | copy buffered stream data to userspace |

## Vendor control transfer

`ioctl(0x100)` is a thin wrapper around a USB vendor control transfer.

Recovered mapping from `AUSBDTV_SendUSBControlTransfer`:

- direction 0 -> `bmRequestType = 0x40` (vendor, host-to-device)
- direction 1 -> `bmRequestType = 0xc0` (vendor, device-to-host)
- command byte 0 -> `bRequest`
- command bytes 1..2 -> little-endian `wValue`
- command bytes 3..4 -> little-endian `wIndex`
- `CxLen` -> `wLength`

The kernel-side DWARF type `_AUSBDTV_USB_CONTROL_TRANSFER_STRUCTURE` is 24 bytes on the recovered x86_64 build:

| offset | member |
|---:|---|
| 0 | `ucTunerNum` |
| 1 | `pCxOut5bytes[5]` |
| 8 | `pCxData` |
| 16 | `CxLen` |
| 18 | `CxDirection` |
| 20 | `TimeOUT` |

The recovered W3U3 Linux userspace paths below set `CxDirection=1`, so their raw transfers use `bmRequestType=0xc0`.

## Control request map

The following request numbers are directly observed in `WDM_cmd.o`. Parameters marked as dynamic are encoded into the four setup bytes (`wValue` / `wIndex`) by the corresponding helper.

| request | recovered operation | notes |
|---:|---|---|
| `0x00` | IR data read | 0x21-byte response buffer |
| `0x01` | IR mode set | mode encoded in setup bytes |
| `0x02` | I2C read | response is status byte + requested data |
| `0x03` | I2C write | write bytes encoded into setup; response status byte |
| `0x04` | channel-filter read | operation selected by wrapper |
| `0x05` | channel-filter write | operation selected by wrapper |
| `0x06` | DSC stop | tuner number encoded in setup |
| `0x07` | DSC start | tuner number encoded in setup |
| `0x08` | GPIO operation | legacy GPIO path; Linux wrapper only emits the set form |
| `0x09` | channel reset | tuner/channel parameters in setup |
| `0x0a` | query USB high-speed state | 1-byte response |
| `0x0c` | customer information read | 58-byte response |
| `0x0d` | I2C staging-buffer fill | used by extended writes |
| `0x0e` | I2C staging-buffer send | used by extended writes |
| `0x10` | GPIOEx set | value/mask in setup |
| `0x11` | GPIOEx get | 1-byte response |
| `0x12` | encryption-chip reset | 1-byte response |
| `0x13` | encryption-register write | register/value in setup |
| `0x14` | I2C write without stop | used by combined I2C sequences |
| `0x17` | system-control read | response is status byte + requested data |
| `0x18` | system-control write | data embedded after request byte |
| `0x19` | I2C read without preceding write | response is status byte + requested data |
| `0x1a` | device random-key read | 16-byte response |

Request values `0x0b`, `0x0f`, `0x15`, and `0x16` are not assigned here because no active Linux W3U3 path in the recovered `WDM_cmd.o` proves their semantics.

## Customer information layout

The PLEX package contains the historical `Customer_Info` layout. Its size is 58 bytes, matching the `0x0c` transfer length:

| bytes | field |
|---:|---|
| 1 | use-customer-info flag |
| 2 | info ID |
| 2 | VID |
| 2 | PID |
| 10 | manufacturer string |
| 16 | product string |
| 15 | HID string |
| 1 | remote-control number |
| 8 | customer-defined data |
| 1 | support-feature bits |

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

## Probe coverage

`asicen-probe` intentionally starts with passive descriptor inspection and three read-style commands:

- `describe`: descriptor-only, no vendor request
- `high-speed`: request `0x0a`, 1 byte
- `customer-info`: request `0x0c`, 58 bytes
- `random-key`: request `0x1a`, 16 bytes

Write/I2C/power/tuner commands remain disabled in the probe until hardware observation validates interface topology and the direct-libusb translation.

## Provenance

Primary binary evidence: PLEX `PX-SERIES_ver.1.0_Linux_Driver.zip`, recovered from the vendor URL on 2026-10-08. The x86_64 kernel module contains DWARF debug information and is not stripped.
