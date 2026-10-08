# PX-W3U3 device lifecycle recovered before hardware bring-up

This document records control-flow facts recovered from the historical PLEX/ASICEN Linux binaries. It does not reproduce the vendor implementation or large register tables.

## Enclosure and USB-function ownership

The W3U3 userspace library explicitly links two `_DEVICE_EXTENSION` objects with `TF_AssignDevExt(dev0, dev1)`. Both device objects receive the same pair of pointers. This is direct evidence that the intended software model is one physical enclosure composed of two runtime USB functions.

Each runtime USB function exposes two local frontend lanes:

- local lane 0: ISDB-S
- local lane 1: ISDB-T

The production daemon should therefore own the enclosure and both USB functions together.

## Device-start GPIO sequence

`DTV_Start` first reads system-control bytes and may skip the cold-start sequence when the device is already initialized. On the cold path the historical code performs a shared GPIO reset/power sequence before frontend initialization.

Observed operations include:

1. clear GPIO mask `0x40`, delay 50 ms
2. clear GPIO mask `0x08`, delay 50 ms
3. set/clear/set GPIO mask `0x10`, with 10 ms delays
4. set/clear/set GPIO mask `0x04`, with 10 ms delays
5. read I2C slave `0xa8`
6. read 16 bytes from slave `0xa8`, register `0xb0`
7. set GPIO value/mask pair `0x27/0xfb`
8. on one cold path also set GPIO `0x40/0x40`

The exact acceptance condition on the system-control read and the meaning of the `0xa8` device still require hardware confirmation.

At the end of `DTV_Start`, the historical code invokes the shared tuner/demod power routine for both local frontend records with the power argument cleared. Later frontend initialization explicitly powers the relevant frontend on.

## Shared frontend power

`TC_PowerTunerDemod` only performs the physical shared-power sequence when invoked for local tuner number 0. This is another indication that power is owned at the USB-function level rather than independently per frontend lane.

The observed power-on sequence toggles GPIO masks `0x04`, `0x05`, `0x10`, and `0x20` with 10 ms delays, then reads slave `0xa8` and terrestrial demodulator register `0x1c`.

If the terrestrial-demod read succeeds, the code modifies register `0x1c` by setting then clearing specific control bits around additional reads/writes.

This sequence is intentionally not enabled in `asicen-probe` until the arriving W3U3 validates the passive topology and read-only vendor commands.

## LNB power

`TC_SetLNB` is simple and shared:

- only local tuner 0 performs the operation
- LNB-on clears GPIO mask `0x20`
- LNB-off sets GPIO mask `0x20`

The electrical polarity should still be verified on hardware before exposing 15 V control to users.

## Frontend initialization

`TC_Initialise` performs a complete two-lane initialization when called for the primary local tuner. It:

1. initializes one demod/frontend profile
2. initializes its RF device
3. changes the internal source selector
4. initializes the second demod/frontend profile
5. initializes its RF device
6. writes terrestrial demodulator register `0x0f = 0x34`
7. makes an initial terrestrial tune call at 773143 kHz with 6 MHz bandwidth

The recovered code contains register tables for both demodulators and the RF devices. Large vendor tables are not copied into this repository; behavior will be reimplemented from protocol facts and hardware observations.

## High-level initialization path

`TF_DTV_Init` maps the enclosure receiver index to an internal tuner object and calls `DTV_Init`, then:

- initializes the polling state
- sets PID-filter boundary values to `0x1fff` / `0x1fff`; the hardware's
  all-PID/pass-through semantics have not been independently established
- starts the stream thread

`DTV_Init` validates the runtime VID/PID against PLEX ASICEN product IDs, performs GPIO/power/reset work, obtains a device random key, initializes encryption/decryption state, and configures frontend/stream state.

For the userland replacement, these responsibilities should be split:

- enclosure/device initialization: `asicend`
- frontend tune/power: hardware backend
- TS transport: async libusb stream backend
- B-CAS/PCSC: separate card backend over the same enclosure owner
- cryptographic TS processing: isolated module with explicit provenance and tests

## Card reader is part of the enclosure

`bBCardInit` uses the same device context, GPIO helper, delays, encryption-chip I2C helpers, and device-random-key path as the tuner stack. It is not modeled as an independent USB reader.

Observed card-init facts include:

- GPIO mask `0x11` reset sequence with 25/50 ms delays
- encryption-chip I2C accesses through addresses around `0x5a/0x5c`
- conditional device-random-key programming
- alternate GPIO mask `0x80` reset path

This reinforces the choice that `asicend` should be the single owner of tuner, GPIO/power, stream, and card transport.

## Hardware acceptance gates

Before enabling state-changing paths:

1. verify one W3U3 enumerates as two runtime `0b06:0005` functions;
2. verify both expose expected bulk endpoints `0x81` and `0x82`;
3. run `high-speed` and `customer-info` read-only probes;
4. validate I2C read against demod addresses `0x30` and `0x32`;
5. capture the original-driver cold-start sequence if practical;
6. only then enable GPIO/power and frontend writes stepwise.
