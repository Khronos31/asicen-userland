# px4-userland compatibility audit, 2026-10-09

Verdict: **distribution compatibility is not satisfied**. Native TS and card
success established reception on Latitude, not the required release matrix.

Inspected ASICEN main `3a06b336f233a0f9e28716678f9b6dc237b857c7` and local
px4-userland `3de7d512cef11756e6f033557ee49917cad2f11c`. The latter's SPEC
10.4, `.github/workflows/build_userland.yml`, `scripts/build-linux-static.sh`
and packaging scripts are the reference. No reference repository was edited.

## Actual executable dependency evidence

Ran `readelf -l`, `readelf -d` and `readelf -V` on the complete Release build
under `out/gcc-release-agent`:

| Binary | Interpreter | DT_NEEDED | GLIBC versions |
| --- | --- | --- | --- |
| asicend | /lib64/ld-linux-x86-64.so.2 | libusb-1.0.so.0, libstdc++.so.6, libgcc_s.so.1, libc.so.6 | present |
| asicenctl | /lib64/ld-linux-x86-64.so.2 | libstdc++.so.6, libgcc_s.so.1, libc.so.6 | present |
| asicen-ts | /lib64/ld-linux-x86-64.so.2 | libstdc++.so.6, libgcc_s.so.1, libc.so.6 | present |
| libifd-asicen.so | none | libstdc++.so.6, libc.so.6 | present |

Thus all three commands depend on glibc, and the daemon dynamically links
libusb. `asicen_libusb` being a CMake STATIC library does not make its
`PkgConfig::LIBUSB` dependency static. CMake currently has no equivalent to
px4's explicit libusb archive override/static production build path.
The IFD is expected to remain a shared plugin, but no matching musl variant
or verified glibc 2.31 floor exists in this build evidence.

The current CI only installs Ubuntu development packages, builds/tests and
runs a probe. It has no multi-platform release-candidate packaging or final
archive dependency audits. There is no basis for claiming any of the nine
required ASICEN binary archives has passed the release gate.

## Required package parity

Product executable mapping is `px4d` → `asicend`, `px4-ts` → `asicen-ts`,
`px4ctl` → `asicenctl`. Linux/macOS need their native card adapter; Android
needs the equivalent Termux USB permission/FD launcher. Additional research
tools do not count toward this parity.

| Platform archive suffix | Reference contract | ASICEN status |
| --- | --- | --- |
| linux-glibc-x86_64 | fully static musl commands + glibc 2.31 IFD | unmet |
| linux-musl-x86_64 | same commands + musl IFD | unmet |
| linux-glibc-aarch64 | fully static musl commands + glibc 2.31 IFD | unmet |
| linux-musl-aarch64 | same commands + musl IFD | unmet |
| darwin-arm64 | system dependencies only, static libusb, PC/SC bundle | unimplemented/unverified |
| android-aarch64 | API24+, static libusb, Termux launcher | unimplemented/unverified |
| android-armv7a | API24+, static libusb, Termux launcher | unimplemented/unverified |
| android-x86_64 | API24+, static libusb, Termux launcher | unimplemented/unverified |
| windows-x86_64 | three commands + native IPC/card path | unimplemented/unverified |

The reference Windows archive currently bundles a libusb DLL. ASICEN's
explicit static-libusb requirement is stricter on that target; copying that
package unchanged would not satisfy it. The reference does not provide a
Windows WinSCard/PCSC IFD in Phase1, so one is not implied by package parity.
The corresponding-source archive, dependency notices, relink verification
and checksum manifest are also missing on the ASICEN side.

## CLI and runtime gaps

- Client argument parsing, finite capture/stdout and exit conventions reuse
  px4 code, and the T27/BS01 live/card evidence remains valid for the tested
  Linux build. This is partial behavioral compatibility.
- Hardware supports only primary receiver0/S or receiver1/T27, one active
  frontend at a time. Parsing T13..T62 does not mean those channels tune.
  Receivers2/3 remain unsupported.
- Hardware `--list`/`--list-json` return an unsupported/not-enumerated result
  with exit0. They are not usable enumeration parity. Explicit primary and
  sibling address/path arguments are still required.
- `--instance` is supported; serial-based `--device` selection cannot be
  copied literally because this ASIC has no USB serial descriptor. IPC wire
  magic/runtime paths intentionally remain product-specific.
- Terminal TS integrity counters remain placeholders; they cannot establish
  reception quality. Startup CC errors have been observed independently.
- `asicend --help` still says only receiver1/T is supported, although the
  implementation now supports primary satellite and the card. The old
  adaptation document also contains stale hardware status; it must not be
  read as the current compatibility verdict.
- Main CLI targets are inside `if(UNIX)`. There is no Windows product build
  path. The hardware backend unconditionally includes `sys/random.h` and
  calls `getrandom`; macOS/Android portability cannot be assumed from copied
  POSIX client code. No Android USB-FD entry path/launcher was established.

## Work required and acceptance checks

1. Adapt px4's established static Linux build, IFD variants, source/relink
   packaging and archive audits rather than inventing a separate contract.
   Require no PT_INTERP/DT_NEEDED/GLIBC references for each final Linux command;
   check static libusb symbols before stripping. A native glibc build passing
   tests is not a substitute.
2. Implement and test the platform boundaries for macOS, Android and Windows,
   including ownership/randomness/IPC/USB access. Build the exact three
   commands and appropriate card/launcher components for all nine archives.
3. Verify full archive inventories and dependency allowlists, matching IFD
   loadability, source/license coverage and source-based relinking. Smoke
   test the extracted candidate binaries on each target runtime.
4. Track CLI gaps separately from binary portability. Keep existing tests;
   add regressions for newly implemented behavior. Requalify reception/card
   use with the actual candidate binaries instead of inheriting development
   binary results silently.

This audit changes the documented acceptance contract only. It does not
claim to have repaired the build, supplied missing platform implementations,
or produced release artifacts. The repository remains private and unreleased.
