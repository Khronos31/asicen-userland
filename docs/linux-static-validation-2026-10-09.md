# Linux static distribution validation, 2026-10-09

The Linux x86_64 private candidates now contain fully static musl commands
with libusb linked into the daemon. Separate archives provide a glibc IFD or
a musl IFD. This is build/package validation, not a nine-platform release or
a new hardware reception result. The repository remains private.

## Inputs and build separation

All three commands and both IFDs were built from one immutable source archive:
`b8abf7c21448a92ad3aba7f7e45973730ff98f39ca9004422f25c317b78c1243`.
The archive contains the implementation and packaging scripts before this
post-build report; its own hash, rather than a claim about a mutable checkout,
identifies the exact source supplied with the candidates.

- Commands: Alpine 3.22.6, musl 1.2.5, GCC/G++ 14.2.0, CMake 3.31.7.
- glibc IFD: Debian 11, glibc 2.31, GCC/G++ 10.2.1, CMake 3.18.4.
- musl IFD: the same Alpine toolchain as the commands.
- libusb 1.0.30 source archive:
  `fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`.
- Vendor firmware: 16,384 bytes,
  `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`.
  It is an external packaging input, not a Git or corresponding-source file.

The environments were isolated filesystem roots under the private work area.
No HA configuration, USB ownership, tuner setting, LNB or firmware upload was
changed. PRoot was updated in that work area because the old version did not
translate the statx calls used by Alpine GNU coreutils. This is a build-host
tool detail, not a runtime dependency of the distributed programs.

`ASICEN_LIBUSB_INCLUDE_DIR` and `ASICEN_LIBUSB_LIBRARY` explicitly select the
static library; `ASICEN_LIBUSB_EXTRA_LIBRARIES` supplies its private link
requirements. `ASICEN_ENABLE_IFD=OFF` disables the host-loaded plugin without
removing the adapter core needed by existing tests. Distribution IFD builds
use `ASICEN_REQUIRE_IFD=ON` so missing PC/SC development files fail explicitly.

## Results

| Item | Result |
| --- | --- |
| Three command ELF files | No PT_INTERP, DT_NEEDED or GLIBC version requirements |
| Actual musl linkage | Compiler/loader/libc hashes recorded; each command's linker map checked against the compiler-selected libc archive |
| Static libusb | Required libusb symbols present in the daemon before stripping |
| glibc IFD | Exports checked; only libc, libpthread and system loader dependencies; highest GLIBC requirement 2.17 |
| musl IFD | Exports checked; only libc.musl-x86_64.so.1 dependency |
| IFD tests | 2/2 in each isolated libc environment |
| Extracted plugin loading | dlopen and IFDHCreateChannelByName lookup passed in both matching environments |
| Extracted commands | Existing mock CLI lifecycle test passed, including finite TS, stdout separation, duplicate instance protection and shutdown |
| Default development graph | 41/41 tests with libusb and IFD enabled |
| Disabled dependency graph | 39/39 with libusb and IFD disabled; affected adapter tests also checked with libusb enabled and IFD disabled |
| Packaging regression tests | 12/12, including rejection paths and a valid source bundle containing directories |
| Final archive verification | Both extracted archives passed inventory, mode, ELF, firmware, license and source correspondence audits |
| Assembly reproducibility | Two glibc assemblies with identical inputs, including one under umask 077, produced identical archive bytes |
| Modified-libusb relink | Rebuilt all three commands from the extracted corresponding-source bundle, changed libusb's version-description string, confirmed that marker in the daemon, and passed the existing mock CLI lifecycle test |

Candidate archive SHA-256 values:

| Archive | SHA-256 |
| --- | --- |
| linux-glibc-x86_64 | `45d431d66ea3ed77302d71ae57ec8d35e9188d0831ceefae88428d0d72e3dffa` |
| linux-musl-x86_64 | `b32a8c011d998e1b3afa24eacbc299dac2788d86e9744a4d8110eb027d90a504` |

Private artifacts and full logs are under `/config/.tools/asicen-work/` as
`asicen-linux-{glibc,musl}-x86_64-candidate-20261009.tar.gz`, with extracted
copies under `extracted-{glibc,musl}-final3-20261009/candidate`.
Each archive carries a complete SHA256SUMS manifest and both build records.

## Source, licensing and firmware

The combined product remains GPLv2 because its inherited px4 code is
GPL-2.0-only and its FC0012 implementation is GPL-2.0-or-later. Existing file
notices remain intact. MIT-only labeling would misstate those permissions.
The packages carry the GPL text, LGPL libusb license and exact source,
musl's complete COPYRIGHT, and GCC's GPLv3 text plus Runtime Library
Exception. See [NOTICES.md](../NOTICES.md).

The corresponding-source bundle contains the exact project snapshot, exact
libusb source archive, runtime notices and executable rebuild/relink steps.
The normal release builder pins pristine libusb; manual CMake overrides allow
recipients to rebuild with a modified libusb. Firmware is unnecessary for
compiling/relinking the commands and IFD.

The relink check changed the version-description string in libusb `core.c`
to `asicen-relink-verification`, rebuilt its static archive, and used the
documented manual CMake override path. No firmware was present in that source
bundle or required by the build. An initial attempt changing `version.h`
instead triggered regeneration of libusb's Autotools inputs and stopped on
missing `aclocal-1.16`; its failure log was retained. Changes to configuration
inputs require the relevant Autotools tools as well as the documented compiler
toolchain. The successful check changed ordinary C source, not configure inputs.

Firmware is included only by the final binary assembler, at its declared
vendor path. Its notice records its provenance and unresolved redistribution
rights; it is not labeled MIT or GPL. External inputs, output paths and
archive inventories are checked. The user's explicit distribution decision
does not establish vendor permission.

## Limits

The previous successful T27/BS01/card trials used development binaries.
Their results are not silently transferred to these static candidates.
Linux aarch64, macOS, Android and Windows still require their own builds and
runtime checks. The required nine-archive matrix is unchanged; this report
does not claim Windows support is complete in either ASICEN or px4-userland.
No public release, version bump or repository visibility change occurred.
