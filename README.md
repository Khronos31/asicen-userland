# ASICEN userland

Development of a direct-libusb userspace driver for ASICEN-based PLEX receivers,
starting with the original PX-W3U3. Based on the protocol skeleton and evidence
from [asicen-research](https://github.com/Khronos31/asicen-research).

This repository is private during development. Public visibility is planned for
the 0.1.0 release. Both USB functions boot and respond to read-only queries.
One terrestrial lane has tuned and locked on the attached PX-W3U3. Finite raw
capture still returns zero bytes; TS reception, satellite reception and B-CAS
decoding are unverified. This is not yet a working four-receiver driver.

## Build and offline tests

Requires a C++17 compiler, CMake, pkg-config, and libusb development headers.

```sh
cmake -S . -B build -DASICEN_ENABLE_LIBUSB=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

`asicend` and `asicen-ts` currently exercise the mock backend. Passing their
integration test does not demonstrate reception from hardware.

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
enablement. This combination has not produced TS on the development machine.

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
The controller register05 setting remains an external test condition and is
not changed by this command.
See [SPEC.md](SPEC.md) for the px4 CLI compatibility target and acceptance gates.
Source attribution and component license details are in [NOTICES.md](NOTICES.md).
