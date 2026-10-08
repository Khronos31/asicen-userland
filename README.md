# ASICEN userland

Development of a direct-libusb userspace driver for ASICEN-based PLEX receivers,
starting with the original PX-W3U3. Based on the protocol skeleton and evidence
from [asicen-research](https://github.com/Khronos31/asicen-research).

This repository is private during development. Public visibility is planned for
the 0.1.0 release. Both USB functions have booted and responded to read-only
queries. One terrestrial lane has tuned and locked on the attached PX-W3U3.
The isolated official Linux environment now captures terrestrial TS; replacing
its faulty multi-block DES routine also yielded valid PAT/PMT and decodable
video after startup. Direct-libusb terrestrial acquisition plus the portable
device-link transform now produces CRC-valid PAT/PMT using a caller-generated
seed and source-backed RF gain adjustment. The px4-derived daemon/client path
also captures real TS on receiver1/T27, including repeated finite captures,
stdout, timed capture and SIGTERM shutdown. Some captures have startup
continuity errors; reception quality is not yet guaranteed.
Satellite reception and the portable B-CAS/recisdb path remain unverified.
This is not yet a working four-receiver driver. See the
[direct receive evidence](docs/reverse-engineering/direct-link-trial-2026-10-09.md).

## Build and offline tests

Requires a C++17 compiler, CMake, pkg-config, and libusb development headers.

```sh
cmake -S . -B build -DASICEN_ENABLE_LIBUSB=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

`asicend`, `asicenctl`, and `asicen-ts` support both an isolated mock backend
and an experimental explicit-target hardware backend. Offline tests alone do
not demonstrate reception; see the [daemon trial](docs/reverse-engineering/daemon-ts-trial-2026-10-09.md).

## px4-compatible command workflow

The portable clients and service are adapted from px4-userland, retaining its
channel syntax, tune timeout, finite capture, output and terminal-error
handling. ASICEN uses `--instance` because this hardware has no USB serial
descriptor. Receivers are `0:S, 1:T, 2:S, 3:T`. Its socket directory and wire
magic are separate from px4-userland.

For an isolated mock trial, create a private runtime directory, run the daemon
in one terminal, and use the clients from another:

```sh
mkdir -m 700 /tmp/asicen-example
build/asicend --mock --runtime-dir /tmp/asicen-example --instance test
```

```sh
build/asicenctl --runtime-dir /tmp/asicen-example --instance test list
build/asicen-ts --runtime-dir /tmp/asicen-example --instance test \
  --receiver 1 --channel T27 --packet-count 100 --output mock.ts
```

The output contains synthetic null packets. Card operations and combined
`status` return unsupported (exit3) until a card backend is attached; they do
not report the physical card as absent.
Use Ctrl-C to stop the foreground daemon. The original `--mock --socket PATH`
research workflow remains available. The [adaptation record](docs/cli-adaptation.md)
documents compatibility, source provenance and the hardware gates.

For the experimental hardware backend, identify both current USB addresses
and physical paths with `asicen-probe list`. Both functions must be available
on the same host and free of another driver or VM owner. Substitute those
observed values below; addresses change after USB re-enumeration:

```sh
build/asicend --hardware --primary BUS:ADDR --primary-port BUS-PORT \
  --sibling BUS:ADDR --sibling-port BUS-PORT \
  --runtime-dir /tmp/asicen-example --instance w3u3
build/asicen-ts --runtime-dir /tmp/asicen-example --instance w3u3 \
  --receiver 1 --channel T27 --packet-count 30000 --output capture.ts
```

Hardware capture currently accepts only receiver1/T27 (557142kHz). It performs
frontend initialization, one RF gain feedback step, acquisition and the
device-link transform internally; the caller does not provide a seed file.
The resulting TS can still be B25-scrambled. Card/recisdb integration, other
channels and receivers, satellite reception and cold-start validation remain
unfinished. Hardware `--list` enumeration is also not implemented; use the
probe tool for USB discovery. Daemon TS quality counters are not yet measured;
use the offline validator rather than interpreting their zero values as proof
of an error-free recording.

## Hardware diagnostics

Start with `build/asicen-probe list` and the explicit
`build/asicen-probe --device BUS:ADDRESS describe` command. Runtime functions
also support `high-speed` and `customer-info` read-only queries.

`load-firmware` requires both `--device BUS:ADDRESS` and `--firmware PATH`.
It changes hardware state and can affect the other function in the enclosure.
Do not use an old USB address after re-enumeration. No automatic firmware
download, driver detach, hub reset, or transfer retry is performed.

Vendor firmware is not included. The extraction script operates on the original
vendor `loader.ko`; vendor binaries and firmware remain locally ignored.

See [HARDWARE-VALIDATION.md](HARDWARE-VALIDATION.md) for provenance, observed
USB topology, transfer results, and unresolved bring-up work.

### Terrestrial frontend diagnostic

`asicen-frontend` performs bounded, explicit-target experiments. Every command
requires a fresh USB address and matching physical port path, verifies the
runtime VID/PID and exclusively claims interface0. It does not detach a kernel
driver, reset the parent hub or enable LNB voltage.

```sh
build/asicen-frontend --device BUS:ADDRESS --port BUS-PORT \
  --frequency-khz 557142 --timeout-ms 20000 --lock-timeout-ms 3000 terrestrial
```

557142kHz locked on the development machine; choose an available local channel
for another installation. Cold-start repeatability and the second terrestrial
lane remain unverified. `lock` can query the demodulator separately.
An opt-in `--shared-demod` on `init`/`terrestrial` also programs the42 original
satellite-demod register facts; it performs no satellite RF tuning or LNB
enablement. Direct TS reception additionally required link preparation and
the RF gain adjustment described in the direct receive evidence above.

After terrestrial tuning, the opt-in `gain-once` command runs one conditional
FC0012 feedback step using live register reads. It is restricted to local1,
does not run periodically, and leaves the gain adjustment active for capture:

```sh
build/asicen-frontend --device BUS:ADDRESS --port BUS-PORT gain-once
```

`capture` saves raw bulk bytes rather than validated MPEG-TS. It requires
`--reset-state 0|1` because the original caller's reset-state default remains
unresolved, together with finite `--seconds` and optional `--output PATH|-`.
Diagnostics use stderr when capturing. Zero-byte captures return failure.
`--queue-depth 4` reserves four asynchronous reads before acquisition starts;
the default depth1 uses synchronous reads. The deadline bounds acquisition,
while cleanup waits for terminal callbacks before freeing transfer buffers.
The experimental `--filter-start --filter-repeat before|after` A/B diagnostic
is restricted to local1, reset-state1 and queue-depth4. It checks terrestrial
lock, repeats the source filter-reset operation at the selected point, logs CF
boundary bytes and elapsed time, and restores/verifies the complete original
CF block after cleanup. It is not a default acquisition requirement; the
source's later polling path does not establish this as a mandatory sequence.
Without `--link-seed-file`, controller register05 remains an external test
condition. The opt-in [link diagnostic](docs/reverse-engineering/direct-link-diagnostic.md)
requires a private16-byte seed file, sets the source-backed output state, and
checks output shutdown after capture. `asicen-transform` then frames and
decodes the saved raw bytes offline. Seed register state cannot be read back;
the diagnostic reports this limitation explicitly.
See [SPEC.md](SPEC.md) for the px4 CLI compatibility target and acceptance gates.
Source attribution and component license details are in [NOTICES.md](NOTICES.md).
