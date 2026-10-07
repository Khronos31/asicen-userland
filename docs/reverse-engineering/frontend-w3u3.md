# PX-W3U3 frontend facts recovered before hardware bring-up

This note separates facts recovered from the historical PLEX/ASICEN Linux binaries from items that still require the physical W3U3.

## Scope

These facts apply to the original PX-W3U3 / W3U2 userspace library recovered from PLEX's historical Linux package. Do not automatically apply them to PX-W3U3 V2; the V2 Windows driver and teardown evidence indicate a later frontend design.

## One runtime USB function = two local frontend lanes

The historical library allocates exactly two `_STnimControl` objects per runtime USB device. The local indexes are 0 and 1.

The dispatch logic in `TC_SetFrequency` proves their broadcast-system roles:

| local lane | system | proof |
|---:|---|---|
| 0 | ISDB-S | frequency values at or below 999999 kHz are rejected for this lane |
| 1 | ISDB-T | frequency values above 999999 kHz are rejected for this lane |

This matches the recovered kernel stream layout: one runtime USB function has two bulk-IN lanes, endpoint `0x81` for local lane 0 and `0x82` for local lane 1.

For W3U3/W3U2, the physical enclosure is expected to contain two such runtime USB functions, giving four simultaneous frontend paths. Hardware enumeration remains the acceptance test.

## Demodulator I2C addresses

The original W3U3 `TunerControl.o` contains fixed demodulator address arrays:

- satellite demodulator: `0x32`
- terrestrial demodulator: `0x30`

These are the 8-bit address values used by the historical library, not normalized 7-bit Linux I2C addresses.

## Terrestrial tuner

The original W3U3 binary contains a named function `FC0012_RSSI_Calibration`, and the terrestrial tune routine programs the same register model. This is strong binary evidence that the original W3U3 terrestrial RF tuner is the Fitipower FC0012 family.

The terrestrial frequency argument is in kHz. A recovered FC0012 branch changes behavior at `260999` kHz.

## Satellite frequency input

The satellite tune routine accepts RF frequency in kHz and performs an exact lookup in a 24-entry table.

### BS

```text
BS01 11727480
BS03 11765840
BS05 11804200
BS07 11842560
BS09 11880920
BS11 11919280
BS13 11957640
BS15 11996000
BS17 12034360
BS19 12072720
BS21 12111080
BS23 12149440
```

### CS

```text
CS02 12291000
CS04 12331000
CS06 12371000
CS08 12411000
CS10 12451000
CS12 12491000
CS14 12531000
CS16 12571000
CS18 12611000
CS20 12651000
CS22 12691000
CS24 12731000
```

`userland/src/channel_plan.cpp` implements these formulas without copying the vendor's per-frequency tuner register table.

## Tuner-through-demod bridge

The tuner register helpers use the demodulator as an I2C bridge/repeater. Recovered paths include operations through demod register `0xfe`, with different bridge command bytes for terrestrial and satellite sources.

Exact state-changing write sequences are deliberately not activated in `asicen-probe` yet.

## Power/init clues not yet enabled

The historical init/power path uses:

- GPIO vendor commands,
- I2C slave `0xa8`,
- terrestrial demodulator register `0x1c`,
- additional tuner/demod initialization tables.

These will be implemented only after passive USB topology and read-only probes are validated on the arriving W3U3.

## Hardware acceptance checks

Confirm:

1. one physical W3U3 enumerates exactly two `0b06:0005` runtime USB functions;
2. both expose bulk IN endpoints `0x81` and `0x82`;
3. local lane 0 maps to satellite and lane 1 to terrestrial;
4. the two internal USB functions can be grouped reliably by USB topology;
5. reset/power behavior of one function does not unexpectedly reset the other;
6. exact original satellite RF tuner identity.

## V2 warning

PX-W3U3 V2 uses runtime USB ID `0b06:0006`, but do not reuse the original W3U3 frontend chip assumptions blindly. Treat V2 as a shared ASICEN transport with a potentially different frontend profile until its official Windows binary or hardware confirms the differences.
