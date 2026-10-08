# Linux ARM64 distribution validation, 2026-10-09

Both ARM64 Linux candidate variants were built and checked. This is **QEMU
user-mode execution**, not native ARM receiver validation. No USB, card,
firmware upload, LNB, HA configuration or system PC/SC service was accessed.

## Inputs and execution environment

The three commands and both IFD plugins use the same immutable source as the
[x86_64 candidates](linux-static-validation-2026-10-09.md):
`b8abf7c21448a92ad3aba7f7e45973730ff98f39ca9004422f25c317b78c1243`.
The pinned libusb 1.0.30 source hash is
`fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf`.
This snapshot precedes the macOS/entropy increment; it is not represented as
the current checkout or a versioned release.

Isolated Alpine ARM64 (GCC 14.2.0, musl 1.2.5) built the static commands and
musl IFD. Debian Bullseye ARM64 (GCC 10.2.1, glibc 2.31) built the glibc IFD.
PRoot at `25dc6a3134891f98a79f57ce1c2c1b23ff15cad1` and QEMU 10.0.13 ran the
ARM binaries on the x86_64 build host. The Debian image layer was
`104799d4ff5b18bad31a13cbbc383730eebef29a1cac161b7905792a8dbe5bd3`.

## Observed results

- All three commands passed the static ELF audit: AArch64, no interpreter,
  no dynamic dependencies and no GLIBC version requirements. Build evidence
  records the selected musl archive and static libusb symbols.
- Both IFD builds passed their two existing offline tests. The glibc plugin
  needs only `libc.so.6` and `libpthread.so.0`, with GLIBC_2.17 requirements.
  The musl plugin needs only `libc.musl-aarch64.so.1`.
- Both final archives passed inventory, matching-source and checksum audits.
  After extraction, each variant passed the existing mock CLI lifecycle
  test. Each extracted IFD loaded in its matching ARM libc environment.
- Firmware was supplied from outside Git at assembly time. The accompanying
  notice retains its unresolved redistribution status; corresponding-source
  archives exclude firmware. These candidates remain local files.

An actual architecture difference exposed an assembler bug: it required a
direct glibc loader dependency on ARM64 as well as x86_64. ARM64 has no such
DT_NEEDED entry in this build. Commit `8dec3f2` corrects both package audits
with architecture-specific exact dependency sets and negative tests for
unexpected libraries and architectures. The glibc archive was assembled with
that corrected script; the binary source snapshot itself remains unchanged.
Reassembling it requires the corrected assembler, rather than the older
assembler inside that snapshot.

| Local archive | SHA-256 |
| --- | --- |
| `asicen-linux-glibc-arm64-candidate-20261009.tar.gz` | `078f108c6bc4d9d290b59d0fe64365a91fbf06779150a33d19d39b672a29cd4a` |
| `asicen-linux-musl-arm64-candidate-20261009.tar.gz` | `821876ff9781d8c7012e15c1f6ba33676991bcda9d38b50f59ca794b13c210c2` |

Builds, logs and extracted files are under `/config/.tools/asicen-work/`, with
`static-arm64-final3-20261009`, `ifd-{glibc,musl}-arm64-final3-20261009` and
`extracted-{glibc,musl}-arm64-final3-20261009` prefixes. A future release must
rebuild a single release revision across the matrix and retain the distinction
between mock/runtime checks and physical receiver support.
