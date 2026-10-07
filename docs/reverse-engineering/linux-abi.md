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
| `0x103` | historical clean-stream ioctl; recovered x86_64 kernel case is a no-op |
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


## Bulk stream transport

The recovered kernel implementation hard-codes two bulk-IN lanes per ASICEN runtime USB function:

| local stream lane | USB endpoint |
|---:|---:|
| 0 | `0x81` |
| 1 | `0x82` |

The endpoint number is built in `AUSBDTV_StartTransfer`: lane 0 selects endpoint 1 with the IN bit, lane 1 selects endpoint 2 with the IN bit.

The start ioctl structure recovered from DWARF is 32 bytes on x86_64:

| offset | member | size/role |
|---:|---|---|
| 0 | `StartStop` | byte; 1 starts, other values stop |
| 1 | `TunerNum` | byte/local stream lane |
| 8 | `TransferObjBufSize` | unsigned long, in 512-byte units |
| 16 | `StreamBufSize` | unsigned long, in 512-byte units |
| 24 | `TransferObjBufNumber` | unsigned long; userspace passes 4 |

The historical userspace path always passes `TransferObjBufNumber = 4`.

Observed production call sites use:
- `TransferObjBufSize = 8`, `StreamBufSize = 0x24b8` (9400), i.e. 4096-byte URB buffers and a 4,812,800-byte kernel ring.
- an alternate/high-throughput path uses `TransferObjBufSize = 0x80` (128), same `StreamBufSize = 0x24b8`, i.e. 65,536-byte URB buffers.

The kernel validates the unit sizes and falls back to 188 / 18800 units if they are outside its accepted range. Those fallback values are not the normal userspace settings.

The historical userspace stream-read API caps one read at `0x2f000` bytes, exactly `188 * 1024` MPEG-TS bytes.

For a direct libusb implementation there is no requirement to reproduce the old kernel ring-buffer sizes exactly. The protocol facts that matter are the bulk-IN endpoints, the two local lanes, and preserving sufficient asynchronous transfer depth/backpressure handling.

### Historical stream-clean ioctl

The userspace library exposes `bCleanStreamBufData`/ioctl `0x103`, but the recovered x86_64 `usbmgr_ioctl` case only reports success and does not mutate stream state. Treat it as a compatibility no-op unless another driver revision proves otherwise.

## I2C read setup encoding

The normal I2C read path is proven end-to-end from `USB_I2C_Read` -> `FUSBDTV_Cmd_I2CRead` -> `bReadI2CData`:

- request: `0x02`
- `wValue.low = slave`
- `wValue.high = register`
- `wIndex.low = mode`
- `wIndex.high = 0`
- USB direction: vendor IN (`0xc0`)
- response length: requested data length + 1
- response byte 0 must equal `1`
- actual I2C bytes start at response byte 1
- high-level reads are chunked to at most 0x20 bytes per request

The no-wait/read-after-write path uses request `0x19`:
- `wValue.low = slave`
- remaining setup bytes zero
- response is likewise status byte + payload.

`USB_I2C_WriteAndRead` first performs a mode-3 write and then a mode-2 read; the latter maps to request `0x19`.
