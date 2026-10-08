# Static distribution increment

Publication-policy update: after this increment, the user authorized making
the development repository public on 2026-10-09, before 0.1.0. The historical
private-work constraint below describes this increment; it is not a current
ban on source publication. Binary release gates remain unchanged.

Objective: implement the missing dependency and packaging contract, retaining
the nine-platform target matrix. Baseline:840dd31. First executable increment
is Linux fully static commands, matching IFD build separation, reproducible
local candidate packaging and audits. Other platform implementations remain
separate gates, not silently omitted or claimed complete.

Acceptance:

1. Build Release `asicend`, `asicen-ts`, `asicenctl` with a musl C++ toolchain
   and an exact static libusb archive. `readelf -l/-d/-V` finds no interpreter,
   NEEDED entries or GLIBC version requirements. Before stripping, daemon
   symbols demonstrate inclusion of libusb. Extracted commands pass help/mock
   smoke on the native host; preserve offline test expectations.
2. Explicit libusb archive/header/extra-private-link overrides; disable IFD
   independently for the fully static command build. Default development
   build and existing tests continue to work. IFD variants remain host-libc
   shared plugins and are separately audited.
3. Candidate packaging includes three commands, correct licensing/notices,
   exact dependency source and rebuild/relink instructions, checksums and
   explicitly supplied firmware. Firmware is never committed or embedded in
   tracked source/generated C++/GitHub workflow logs. Hash/size are metadata.
4. Firmware is a separate vendor component, not MIT/GPL licensed by this
   project. The user explicitly chose redistribution on2026-10-09 with rights
   unresolved. Record this accurately; don't label rights cleared or add a
   new approval gate. Packaging is local/private; publication remains at0.1.0.
5. No MIT relabeling of inherited GPL code. Retain GPL-2.0-only px4 and
   GPL-2.0-or-later frontend notices, combined GPLv2 product license and
   libusb LGPL source/relink materials.

Prior art: adapt `/config/GitHub/px4-userland/scripts/build-linux-static.sh`
and packaging/audit design. Its firmware exclusion is superseded by the
ASICEN user's explicit inclusion requirement; its reference Windows work is
in progress. No wholesale port of hardware code or Windows DLL exception.

Constraints: no hardware I/O, VM USB handoff, HA changes/restarts, public
release, version bump, license removal or edits to px4-userland. Work in this
repo and `/config/.tools/asicen-work/`; use existing toolchains read-only or
create isolated build environments. Don't use unrelated production containers.

Rollback: tracked source changes are confined to build/packaging/docs and new
tests, recoverable by reverting this increment. Generated private candidates
are outside Git. Existing firmware and development artifacts stay untouched.

## Accepted review corrections

- Empty ELF dependencies alone do not prove musl: identify and record the
  musl compiler/sysroot/libc inputs, and reject a static-glibc certification.
- Exercise IFD ON/OFF with tests ON on a machine with PC/SC headers; retain
  the IPC adapter core when existing tests need it.
- Each Linux archive requires its matching IFD, architecture, exported IFD
  entry points, libc/version allowlist and extracted-artifact checks. Do not
  label a modern glibc plugin as satisfying the2.31 floor.
- Build from the exact immutable source snapshot packaged with the binary;
  record source/dependency/toolchain/binary hashes and rebuild from extracted
  source. Arbitrary modified libusb source cannot claim pristine provenance.
- Release staging/output and supplied firmware must be outside the repo,
  with symlink/path checks. The existing ignored firmware is copied outside
  before packaging. Source inventory excludes it; only the binary archive's
  declared vendor path may contain it. Test these failure cases.
