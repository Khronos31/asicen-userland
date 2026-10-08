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
into this project. No publication or release has been made.
