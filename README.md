# ASICEN userland

Development of a direct-libusb userspace driver for ASICEN-based PLEX receivers,
starting with the original PX-W3U3. Based on the protocol skeleton and evidence
from [asicen-research](https://github.com/Khronos31/asicen-research).

This repository is private during development. Public visibility is planned for
the 0.1.0 release. This is not yet a working four-receiver driver: the first USB
function boots and responds to read-only queries, while booting it disconnects
the sibling function. Tuning and real TS reception are not implemented.

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
