# Satellite frontend diagnostic, 2026-10-09

The direct-libusb diagnostic on Latitude completed BS01 tuning, lock,
advertised-TSID reading and slot0 selection/readback. It did not capture
satellite TS and does not establish satellite recording support.

The source snapshot used is privately retained as
`satellite-diagnostic-source-20261009.tar.gz`; this trial preceded the card
backend changes. Both USB functions were detached from the idle CentOS VM,
then claimed together on AnduinOS (kernel7.0.0-34-generic).

Source snapshot SHA-256:
`ed99e9fe0cf106af75a0c37791229c1b38cb6c94ddf94dc1194287500c0a8ad4`.

```sh
asicend --hardware --primary 1:102 --primary-port 1-2.1 \
  --sibling 1:114 --sibling-port 1-2.2 \
  --probe-satellite 11727480 --slot 0
```

These USB addresses are historical observations, not defaults. The command
returned0 in0.566seconds, reported lock1, three nonempty TSID slots and
successful selection/readback. Shared initialization included both demod
tables. No LNB supply setting was changed or measured. This does not prove
which external antenna-power condition enabled reception.

## Functional recovery check

After the satellite diagnostic and its shutdown, a fresh `asicend --hardware`
and the ordinary px4-derived `asicen-ts --receiver 1 --channel T27` captured
30000packets (5640000bytes). Both client and daemon returned0.

Independent validation found:

- 30000 valid packets, zero invalid sync, TEI or trailing bytes.
- Zero continuity discontinuities, duplicates or conflicting duplicate CCs.
- PAT28/28 and PMT62/62 sections with valid CRCs.
- 28915 scrambled packets; card/B25 decoding was not part of this test.

This confirms terrestrial functional recovery in the tested session, not
restoration of every original RF register.

## Trace and retention

USB tracing began before the diagnostic frontend initialization and covered
the terrestrial regression and cleanup. tcpdump captured3996records with
zero kernel drops and was explicitly stopped. The trace contains no GPIO
operation whose mask includes LNB bit20 and no GPIOEx write. First and last
GPIO responses areff; the last controller05 read is00.

Raw USB trace and TS are private, including a persistent0600 archive at
`/config/.tools/asicen-work/satellite-probe-private-20261009.tar`. No card IDs,
keys, raw TS or raw trace are committed. Both USB functions remain idle on
the host for the next bounded card diagnostic at this checkpoint.
