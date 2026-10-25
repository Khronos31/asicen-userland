# Source provenance

## Default license scope

The combined product is GPL version 2. A file that carries its own license
notice keeps that notice. Where a file in this repository has no more specific
notice, it is part of the GPL-2.0-only product work. These locations are
specific:

- `third_party/px4-userland/` is GPL-2.0-only, from px4-userland `dd1c1bea16a5871085d5f2a42c009364c1ec7345`.
  Local changes are marked in the modified files and in `UPSTREAM.md`.
- The embedded release publisher/verifier, tests and adapted workflows are
  GPL-2.0-only from px4-userland `b4635226082161ce09f811b1571c9dd25f40cf03`.
  Their SPDX notices remain intact. The exact upstream license is preserved in
  `third_party/release-automation/LICENSE`; the retained origin and local
  adaptations are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md#embedded-release-automation).
- The FC0012 tuner implementation is GPL-2.0-or-later.
- `userland/src/v2_frontend.cpp` and `userland/src/v2_nmi.cpp` are the
  GPL-2.0-only adaptation described below. Upstream did not state a version.
- libusb 1.0.30, when unpacked for a build, stays LGPL-2.1-or-later.
- `firmware/` is a vendor component and is not covered by this project's GPL.
- `distribution/licenses/` holds upstream license texts for the toolchains.

This scope is a statement of which notice applies. It is not a clearance
opinion, and it does not relicense a file that already has its own notice.

`userland/src/frontend_sequence.cpp` and its header declare
`GPL-2.0-or-later`. The FC0012 algorithm follows Linux v6.6
[`drivers/media/tuners/fc0012.c`](https://github.com/torvalds/linux/blob/v6.6/drivers/media/tuners/fc0012.c),
Copyright (C) 2012 Hans-Frieder Vogt. The original attribution is retained in
both files; the GPL version2 text is supplied in `COPYING.gpl2`.

The short demodulator/tuner register-value lists and USB request layouts are
protocol facts recovered by static inspection of the original PLEX driver
artifacts. Source files and hardware reports name the object members and
function offsets used. Vendor object code, application license seeds and proprietary cryptographic
tables are not included. The separately tracked loader firmware and its
unresolved redistribution status are described below. Historical USB traces
may retain truncated firmware payload bytes; they are not a new source license.

The portable CLI/IPC/service subset and header-only TS continuity checker of
px4-userland are incorporated from official release v0.2.1,
revision `dd1c1bea16a5871085d5f2a42c009364c1ec7345` under GPL-2.0-only.
Imported files retain their SPDX notices; its license is reproduced in
`third_party/px4-userland/LICENSE`. The file inventory and local changes are
recorded in [the source manifest](third_party/px4-userland/UPSTREAM.md). The adapted CLI
entry points `userland/tools/asicenctl.cpp`, `userland/tools/asicen_ts.cpp`,
and `userland/tools/asicen_ts_windows.cpp` retain that GPL-2.0-only license.
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

On 2026-10-09 the user selected inclusion of the Linux loader image in binary
distribution archives, and later the same day directed that image into Git so
GitHub-hosted runners can copy it. The file is `firmware/asicen-loader.bin`
(16384 bytes, SHA-256
`b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`). The
accepted common default image is also tracked at
`firmware/asicen-loader-common.bin` (16384 bytes, SHA-256
`5db39facda140bdcb3a238dad10da5aac752a1ba3aaf17e2239f8d79a0ad6a2a`). Both
images are included in binary distribution archives; corresponding-source
archives and `git archive` omit `firmware/`.

The user reaffirmed this ASICEN-specific preservation and redistribution policy
on 2026-10-11 JST because continued availability from PLEX is uncertain.
This is an explicit project-policy difference from px4-userland, not evidence
of a redistribution grant. The Linux image and provenance remain preserved;
the common image is extracted from the verified Windows BDA x64 loader
described in the archive notice.

The firmware is a separate vendor component: this project's GPL license and
any licenses on newly written packaging code do not grant rights to it.
Redistribution rights have not been established. A vendor-component notice
must accompany its binary-package copy and state the unresolved status,
source artifact, extracted object, byte size and SHA-256. Do not label it MIT,
GPL, public domain or rights-cleared.

The PLEX Linux driver ZIP's `loader.ko` and `as11usbdtv.ko` carry
`license=GPL` module metadata. The 64-bit loader
(SHA-256 `10ad321dd47d93f89fde556ec8683b7a8ce0fcc74cd90e4a04308592dc9719f0`)
embeds this FirmBin at file offset `0x1b00`. DWARF names the metadata at
`devMgr.c:119` and FirmBin at `firmbin.c:3`. That evidence does not decide
whether the extracted firmware is a GPL work or is outside the GPL. The
inspected official Linux and Windows packages do not contain an explicit
firmware redistribution permission. This project's GPL notice cannot grant
that permission.
An accompanying full license text establishing the official firmware's grant
was not found in that inspection. Module metadata is recorded as evidence,
not as confirmation that the firmware itself is GPL-licensed.

## NMI / TDA2014x frontend adaptation

`userland/src/v2_frontend.cpp` and `userland/src/v2_nmi.cpp` adapt portable
parts of driver algorithms by Budi Rachmanto / AreMa Inc., from
[`knight-rider/ptx` revision ad3dc2619787a9a38ae3c5a17137f47d9631e8e1](https://github.com/knight-rider/ptx/tree/ad3dc2619787a9a38ae3c5a17137f47d9631e8e1):
`drivers/media/tuners/nm131.c` and `tda2014x.c`. Those two files carry a
copyright line and `MODULE_LICENSE("GPL")`. No SPDX identifier, no GPL version,
and no separate license document for those drivers was found in that revision.
The repository's recpt1 `COPYING` is not a license for them. Linux's
[module-license rules](https://docs.kernel.org/process/license-rules.html#module-license)
say `MODULE_LICENSE` is not a substitute for the source license. Attribution
and the source checksums are kept in the local files and in
[the V2 source facts](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/reverse-engineering/v2-nmi-families.md). The local adaptation is
distributed as GPL-2.0-only, which is this product's license and the stricter
reading of an unspecified GPL version. That is not a finding that upstream
named GPL-2.0-only. Asking upstream remains open. Model-specific differences
were cross-checked against official Windows object code by static inspection;
no vendor executable code or application-key tables are incorporated.

The separately supplied Windows loader firmware is not covered by these source
licenses. The extraction script reads its decoding key from the user's verified
vendor file; it embeds neither the key nor firmware. Its separate fingerprint
and load address do not grant redistribution rights or make it interchangeable
with the Linux firmware image.
