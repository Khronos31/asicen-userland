# Direct USB daemon TS trial, 2026-10-09

Revision `f1caa70` captured terrestrial TS through `asicend --hardware` and
the px4-derived `asicen-ts` client on Latitude (AnduinOS, kernel
7.0.0-34-generic). No official SDK or legacy kernel module was used in this
capture process. The official objects remain the protocol/reference source.
This demonstrates receiver1/T27 only, not four-receiver or B25 completion.

## Conditions and commands

Both USB functions were detached from the idle CentOS verification VM before
ownership by the host daemon. Primary was bus1/address102, port1-2.1;
sibling was bus1/address113, port1-2.2. These are historical addresses, not
reusable defaults. USB tracing began before this daemon's frontend power-on.
This was not a physical cold boot or firmware-load trace.

```sh
asicend --hardware --primary 1:102 --primary-port 1-2.1 \
  --sibling 1:113 --sibling-port 1-2.2 \
  --runtime-dir PRIVATE/runtime --instance w3u3
asicen-ts --runtime-dir PRIVATE/runtime --instance w3u3 \
  --receiver 1 --channel T27 --packet-count 30000 --output PRIVATE/count-first.ts
```

The same daemon handled five successive acquisition sessions. Each prepared
its own private random link seed. The implementation uses shared demod
initialization, T27 tuning and lock, one RF gain feedback step, reset-state1,
PID boundaries1fff/1fff, four4096-byte transfers, DSC06, filter-start and
controller output a0 with the link transform. Clients receive188-byte TS.
No fixed startup-packet discard was added to hide discontinuities.

## Measured results

The independent `validate_capture_ts.py` examined every output packet.
All five files had zero sync errors, zero TEI and zero trailing bytes.
Every assembled PAT/PMT section had valid CRC; B25 scrambling remains.

| Trial | Bytes | Packets | CC discontinuities | Conflicting duplicate CC | Valid PAT / PMT |
| --- | ---: | ---: | ---: | ---: | ---: |
| First count | 5,640,000 | 30,000 | 0 | 0 | 28 / 63 |
| Second count | 5,640,000 | 30,000 | 38 | 0 | 27 / 60 |
| stdout count | 564,000 | 3,000 | 9 | 1 | 3 / 6 |
| 3-second duration | 6,372,824 | 33,898 | 0 | 0 | 31 / 70 |
| Daemon interrupted during capture | 622,468 | 3,311 | 0 | 0 | 3 / 6 |

The second capture's last continuity event was at zero-based packet351;
stdout's was at packet60. This locates the observed errors near startup,
but does not establish their hardware/software cause or guarantee steady
state quality in longer recordings. The daemon's TS quality counters are
not implemented measurements; their zero values must not override this
offline finding. No B25 decode is claimed for these files.

Both count clients, stdout and duration returned0. The duplicate daemon
returned4 (busy), `asicenctl list` returned0, and `card-status` returned3
(unsupported). SIGTERM during the final capture made the daemon cleanly
return0 and its interrupted client return7; this terminal error was preserved
instead of reporting a successful full-duration recording.

Compact counts and SHA-256 values are in
[the validation record](../hardware-traces/2026-10-09-daemon-ts.validation.json).
Latitude's GCC Release build passed all34 CTest tests. Earlier isolated
checks also covered no-libusb builds and sanitizer session/IPC/drain tests.

## Shutdown and ownership evidence

The host trace contains five DSC06 starts and five DSC07 stops. Controller05
writes alternate a0/00 five times; the corresponding reads confirm each
enabled and disabled state with USB status0 and two-byte responses.
All15 complete CF00–44 reads in the trace agree, covering pre-setup snapshots
and restoration readbacks. The final GPIO read is ff, matching the initial
snapshot, and an independent read after daemon exit also returned ff.
There are no GPIO writes whose mask includes LNB bit20 and no GPIOEx writes.
Seed-register clearing is acknowledged by the device but cannot be read back;
this is not a claim of verified seed erasure.

tcpdump captured12,304 packets with zero kernel drops and was explicitly
stopped. Both USB functions were returned to the CentOS VM afterward, where
both enumerated as0b06:0005 and as11usbdtv had use count0. No reception harness
or monitor was left running. Raw TS, pcap (which contains seed material),
full validations and logs are retained privately outside Git, including a
persistent `/config/.tools/asicen-work/daemon-hardware-private-20261009.tar`
backup. Only sanitized measurements are committed.

## Failures resolved before this successful run

- Optimized GCC linked an unused native px4 card adapter into the portable
  profile; the profile now excludes that adapter without importing IT930x.
- Root opening an existing user-owned enclosure lock with O_CREAT hit
  protected-regular-file policy. Existing lock files are now opened without
  O_CREAT, retaining ownership, inode and policy protections.
- Controller I2C identity/idle checks were incorrectly performed before
  frontend power-on. The bridge revision and GPIO snapshot remain pre-power;
  controller checks now run after bounded power-on, before shared demod init.
  Failed initialization restores GPIO and failed restoration quarantines the
  backend. The first fix missed a second pre-power idle check; f1caa70 removes
  that remaining check. These failed trials stopped before frontend writes.

Next work is to diagnose startup continuity, implement actual quality
counters, expand channel/receiver coverage, and connect the ASICEN card
mailbox to the portable card/recisdb path. Public release, cold-start claims,
satellite reception and LNB testing remain outside this result.
