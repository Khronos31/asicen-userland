# Source provenance

`userland/src/frontend_sequence.cpp` and its header declare
`GPL-2.0-or-later`. The FC0012 algorithm follows Linux v6.6
[`drivers/media/tuners/fc0012.c`](https://github.com/torvalds/linux/blob/v6.6/drivers/media/tuners/fc0012.c),
Copyright (C) 2012 Hans-Frieder Vogt. The original attribution is retained in
both files; the GPL version2 text is supplied in `COPYING.gpl2`.

The short demodulator/tuner register-value lists and USB request layouts are
protocol facts recovered by static inspection of the original PLEX driver
artifacts. Source files and hardware reports name the object members and
function offsets used. Vendor object code, firmware, application license seeds
and cryptographic tables are not included in this repository.

The portable CLI/IPC/service subset of px4-userland is incorporated from
revision `d51c83e1d7eeb829fd61f87f6ea93ad2043b9d00` under GPL-2.0-only.
Imported files retain their SPDX notices; its license is reproduced in
`third_party/px4-userland/LICENSE`. The file inventory and local changes are
recorded in [docs/cli-adaptation.md](docs/cli-adaptation.md). The adapted CLI
entry points `asicenctl.cpp` and `asicen_ts_portable.cpp` retain that license.
The combined CLI executables therefore use GPL version2; existing
GPL-2.0-or-later source notices remain unchanged. No IT930x hardware backend
is linked into the ASICEN commands.

The independently implemented transport transform uses standard DES tables
and source-inspected protocol permutations, not vendor lookup-table blobs or
application-license tables. Its synthetic reference oracle requires local
vendor objects only for an optional offline comparison, never for the product.

recisdb-rs remains a separate downstream program; none of its source is linked
into this project. Public development source was authorized on 2026-10-09;
no versioned binary release has been made.

## Product license and binary dependencies

The combined product is distributed under GPL version2. MIT-only licensing
is not available for this existing combination: it includes GPL-2.0-only
px4 source and a GPL-2.0-or-later Linux FC0012 implementation. No inherited
copyright or license notice is replaced. This does not impose a fee or a
noncommercial restriction; recipients receive the corresponding source and
build instructions under the retained terms. Independently authored files
retain their existing notices rather than being indiscriminately relicensed.

libusb remains LGPL-2.1-or-later. Static distribution must include its exact
license, source provenance and source/rebuild/relink materials; a static
binary does not make libusb MIT or transfer ownership of it. Toolchain/runtime
licenses must also be carried with their applicable binary packages.
See [the libusb license](https://github.com/libusb/libusb/blob/v1.0.30/COPYING).

The Linux static toolchain uses musl 1.2.5; include its complete COPYRIGHT
file, including component attributions, from the
[official release](https://musl.libc.org/releases/musl-1.2.5.tar.gz).
The GCC runtimes use GPLv3 with the GCC Runtime Library Exception 3.1;
include both COPYING3 and COPYING.RUNTIME from the matching GCC release.
The exception, rather than an MIT relabeling of those runtimes, permits
their inclusion under the applicable conditions. The build records the
compiler, libc and license-file hashes. The reference license sources are
[GCC 14.2.0](https://github.com/gcc-mirror/gcc/tree/releases/gcc-14.2.0)
and [GCC 10.2.0](https://github.com/gcc-mirror/gcc/tree/releases/gcc-10.2.0).

## Vendor firmware in binary packages

On 2026-10-09 the user explicitly selected firmware inclusion in distribution
archives while keeping it out of Git. The repository contains extraction and
packaging code plus metadata, not the vendor firmware payload. The firmware
is supplied externally to candidate packaging and is excluded from the
corresponding-source archive.

The firmware is a separate vendor component: this project's GPL license and
any licenses on newly written packaging code do not grant rights to it.
Redistribution rights have not been established. A vendor-component notice
must accompany its binary-package copy and state the unresolved status,
source artifact, extracted object, byte size and SHA-256. Do not label it MIT,
GPL, public domain or rights-cleared. This records the user's distribution
decision without claiming permission from the vendor.
