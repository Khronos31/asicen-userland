# Remaining distribution targets

The nine-platform contract remains the objective. Public source development
is now authorized; release tags and claims of verified receiver support are
separate from source publication. Linux x86_64 has verified static candidates;
the same Linux source is being tested on aarch64.

## Next bounded increment: macOS arm64

Produce the three commands with an exact static libusb 1.0.30 and a native
PC/SC IFD bundle. Reuse the px4 build/adapter conventions without importing
its receiver backend. Keep the public CLI behavior and error semantics.

Replace the hardware backend's Linux-only getrandom call with a narrowly
scoped secure entropy abstraction. POSIX implementations must handle partial
reads, EINTR and errors, close resources, and never fall back to predictable
values. Preserve the sixteen-byte seed and current cleanup-on-failure flow.
Do not tie hardware entropy to IPC identities or change device protocols.

Use the Apple SDK PCSC headers/framework and the required bundle manifest.
Keep Linux IFD detection and ON/OFF behavior intact. No daemon installation,
system PC/SC configuration, attached device access or LNB action is permitted
in the build/runtime tests.

Acceptance:

1. Native arm64 build and offline tests on a standard macos-14 Actions runner.
2. All three command binaries contain no dynamic libusb or Homebrew runtime
   path; Mach-O dependencies are limited to the required system libraries and
   frameworks. Record static libusb symbols before stripping.
3. The extracted IFD bundle has the required exports and loads on that runner.
4. Extracted commands pass the existing mock CLI lifecycle test; no hardware
   reception or macOS USB-driver takeover is implied by that test.
5. Build from the shipped immutable project source and pinned libusb source,
   with source/relink instructions, checksums and complete applicable notices.
6. The final package assembler accepts firmware only from an external path;
   CI must not place it into Git or publish whole vendor driver archives.
7. Keep Linux tests unchanged; add focused tests for entropy error/cleanup
   paths and actual platform packaging, without weakening existing fixtures.

Subsequent increments retain Android API24+ aarch64/armv7a/x86_64 with a tested
Termux USB-FD launcher, and Windows x86_64 with native IPC/ownership handling
and static libusb. The reference Windows implementation is still in progress.
Cross-compilation alone does not establish target runtime or hardware success.

Rollback: revert the platform build/entropy changes; isolated build outputs
and CI candidates can be replaced. No production configuration is involved.

## Accepted review checks

- Preserve Linux's existing getrandom(..., 0) behavior; use platform-specific
  secure entropy on the other targets. Cover short fills, interruption, zero
  return, cleanup and exact sixteen-byte success. A failure must clear partial
  bytes and retain the hardware preparation caller's fail_prepare path before
  stream/output setup. Use existing test seams where possible; do not redesign
  the hardware backend merely to introduce a test seam.
- Check the extracted IFD bundle's asicen identity, CFBundleExecutable path,
  arm64 architecture and every required export. Exercise its asicen-userland
  prefix against isolated mock IPC, without system PC/SC registration.
- Execute the shipped source/relink recipe in a fresh directory on the macOS
  runner; inspect every command and the IFD for architecture, dependency and
  runtime-search-path violations. Build from the exact shipped snapshot.
