# PC/SC and recisdb decoding trial, 2026-10-09

The direct W3U3 mailbox implementation successfully decoded the recorded
T27 transport stream through ASICEN IPC, the imported PC/SC IFD, pcsc-lite
and recisdb1.2.4. It used the inserted card, not supplied working keys.
The implementation was committed as3742842 after the trial.

Source snapshot SHA-256:
`e20c8dd869f57bcec3937a2b4a368a9e1b4ba8afbef81bab12fd0e5d197e5723`.

## Conditions

- Latitude: AnduinOS, kernel7.0.0-34-generic, pcsc-lite2.4.1.
- Both USB functions claimed by `asicend --hardware --card-only`.
- Primary1:102/port1-2.1, sibling1:114/port1-2.2 (historical addresses).
- Private runtime/instance and reader configuration, using `libifd-asicen.so`.
- Separate mount namespace: private socket directory bound over `/run/pcscd`.
- `PCSCLITE_HP_DROPDIR` pointed to an empty private directory; client
  `PCSCLITE_CSOCK_NAME` selected the private socket. No system service change.
- pcscd ran foreground with error-level logging and APDU logging disabled.

The reader's DEVICENAME used
`asicen-userland:runtime=PRIVATE/runtime:instance=w3u3:access=user`.
The decoder command was run twice against the same captured file:

```sh
recisdb decode --input PRIVATE/regression.ts --no-strip PRIVATE/decode.ts
```

The input is the satellite-diagnostic follow-up T27 capture documented in
[the frontend trial](satellite-frontend-trial-2026-10-09.md).

## Results

Both independent recisdb processes returned0 and produced identical output:

| Measurement | Input | Each decoded output |
| --- | ---: | ---: |
| Bytes | 5,640,000 | 5,640,000 |
| Packets | 30,000 | 30,000 |
| Scrambled packets | 28,915 | 0 |
| Invalid sync / TEI / CC discontinuities | 0 / 0 / 0 | 0 / 0 / 0 |
| CRC-valid PAT / PMT sections | 28 / 62 | 28 / 62 |

Both output SHA-256 values are
`338d19e521316ce4e00ead20ae6f8ec982d7ef7c51ee2520a34e628187a24899`.
ffmpeg decoded one video frame from the second output, confirmed by its
progress counter, and returned0. This goes beyond observing cleared TS bits.

## Cleanup and limits

pcscd, asicend and tcpdump all exited0. No related process remained.
System pcscd.service remained inactive and pcscd.socket remained active.
The trace captured1318records with zero kernel drops and zero matched
vendor-control completion errors. It contains ten card submits and eleven
consumes (including ATR). GPIO first/last values areff, last controller05
read is00, and no LNB-mask or GPIOEx write occurred.

Raw TS, card traffic, logs and full validations are private, backed up in
`/config/.tools/asicen-work/recisdb-private-20261009.tar` with mode0600.
Both USB functions are idle on the host pending the satellite TS increment.

This is **offline decoding through card-only mode**. Simultaneous capture
and card traffic, hot removal/reinsertion, satellite TS and secondary USB
receivers are not established by this result. Release/libusb-ON tests passed
39/39 and OFF tests37/37; Latitude's focused IFD/card tests passed5/5.
