# W3U3 hardware handoff for Codex

Goal: turn the current enumeration/protocol skeleton into a direct libusb bring-up without guessing.

## Before sending any control transfer

1. Build `asicen-probe` and run it with the W3U3 attached.
2. Save `lsusb`, `lsusb -t`, and `lsusb -v -d 0b06:0005` output.
3. Record how many `0b06:0005` devices appear for one physical enclosure and their bus/port topology.
4. Record interface/endpoint descriptors, especially bulk IN endpoints and interface numbers.
5. Confirm whether the device first appears as loader `1738:5211`/`5216` and re-enumerates as `0b06:0005`.

## Safe first implementation targets

1. Open/claim runtime interface(s).
2. Implement generic vendor control helper from `docs/reverse-engineering/linux-abi.md`.
3. Start with read-only requests whose semantics are known:
   - device high-speed query (`bRequest 0x0a`)
   - customer info (`0x0c`)
   - random-key read (`0x1a`)
4. Compare replies against the historical userspace library behavior.
5. Only after that, implement I2C and tuner power/tune paths.

## Evidence to collect

- usbmon/Wireshark USB capture from the official driver if a compatible VM can run it
- all control setup packets and returned lengths
- bulk endpoint number(s), packet size and transfer cadence
- re-enumeration behavior after firmware download
- mapping between physical tuner connector and `(USB function, local tuner)`

## Architecture target

Production shape:

```text
asicend
  owns all USB functions for one enclosure
  owns receiver leases / shared GPIO / power / card state
    |
    +-- asicen-ts
    +-- asicenctl (later)
    +-- PC/SC IFD (later)
```

Development path:

`asicen-probe` directly owns USB and exposes low-level diagnostics. Keep this path even after `asicend` exists.

## Do not assume

- do not assume W3U3 V2 uses identical request semantics merely because its Windows driver shares files
- do not assume endpoint numbers from disassembly until descriptors confirm them
- do not assume two internal USB functions can be independently reset without affecting the enclosure
- do not port vendor binary implementation verbatim; reimplement behavior from observed ABI/protocol facts
