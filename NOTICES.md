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

px4-userland and recisdb-rs currently serve as references. The diagnostic
increment does not incorporate their source or link recisdb-rs. Record their
licenses and retained notices before any later source reuse; release licensing
for the complete CLI suite remains to be finalized before publication.
