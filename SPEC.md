# ASICEN userland development contract

Status: acceptance plan, updated 2026-10-13 for reuse of official
px4-userland v0.2.1 `dd1c1bea16a5871085d5f2a42c009364c1ec7345`.
This is not a release/support claim.

Release-procedure change (2026-10-11): share PX4's release verification and
publication implementation and matrix/record structure while preserving ASICEN's
existing hardware acceptance, fixed 0.1.0 soak decisions, firmware policy and
Windows distribution exclusion. For 0.1.0, embed the reviewed GPL-2.0-only
automation locally with recorded provenance; do not wait for PX4's next
WinSCard release. A future external dependency must use the full commit SHA
resolved from an official PX4 release tag. This change does not authorize a release or
complete a hardware gate. The code-parity pin used by that earlier change was
`1a1485d0c3e972e0a47be907edb67949564aa9a7`.

Reusable-code change (2026-10-13): refresh the existing portable PX4 subset to
the official v0.2.1 commit above, including its shared CC checker and opt-in
card-generation IPC contract. This is an implementation/observation change
before the next candidate is frozen. The product VERSION stays 0.1.0; historical
hardware evidence does not qualify the changed binaries. The embedded release
automation has its own provenance and remains separate from this C++ pin.

Receive-performance revision (2026-10-13): use four 65,536-byte asynchronous
bulk-IN transfers in the ASICEN backend. This selects the official
[high-throughput buffer size](docs/reverse-engineering/linux-abi.md), also
observed as four pending transfers in the
[guarded official trace](docs/hardware-traces/2026-10-08-guarded-init.usbmon.txt).
Those historical submissions do not prove successful reception or qualify a
new candidate. Preserve transfer ordering, draining, backpressure, the existing
four-packet sync probe and all model/access-path support boundaries.

The portable mode-7 transform may prepare its two DES schedules once per
decoder and use fused S/P lookup tables, provided packet bytes, key boundaries,
seed derivation, XOR order, invalid-input behavior and decoder counters remain
identical to the original scalar implementation. Keep the scalar oracle and
synthetic timing tool test-only; timing is not a pass/fail acceptance gate.
The pinned PX4 packet/card contracts below, CLI, IPC and firmware are unchanged.
This revision changes candidate binaries and therefore requires a new candidate
and its existing release-validation gates; it is not a hardware acceptance,
support expansion, release-note request or tag/publication authorization.

Terrestrial readiness revision (2026-10-14 JST): for the original W3U3/W3U2
FC0012 path, do not report acquisition readiness from the lock observation
made before gain adjustment. Require a fresh locked observation after gain,
then a locked observation at least 340 ms later; an observed unlock resets
that interval. Preserve the shared outer deadline and cancellation, and reset
readiness on retune and close. This is a hardware-specific conservative
candidate, supported by the E02 warm T27 A/B results in Issue #7 (A: 2/15
failures, B: 0/15), not proof of a vendor-required minimum or universal fix.
The diagnostic override remains temporary; unset uses this readiness behavior
without tracing. Do not change LNA values, shared PX4 services, TS bytes or
validation rules. Existing evidence does not qualify the changed binaries;
cold firmware-transfer IO remains a separate unresolved issue.

Sibling loader acquisition revision (2026-10-14 JST): after a successful
primary upload and sibling GPIO restore on a dual-function device, treat
explicit transient sibling open failures as acquisition failures eligible for
fresh topology resolution within the existing bounded acquisition deadline.
Preserve permanent-error handling, cancellation, identity checks and warm/single
device behavior. Do not retry any firmware request, repeat the primary upload
or GPIO restore, or accept a failed transfer as success. This targets the
pre-transfer open failure observed in Issue #7 P01; it does not establish the
cause of the earlier transfer-stage IO error. After offline validation, compare
the fixed build using C10 and P10. If neither series reproduces the failure,
record only "修正後の簡易試験では再現せず" and proceed under the user's decision,
without claiming a statistically established failure rate or guaranteed cure.

## Objective

Receive real PX-W3U3 MPEG-TS with a direct libusb userland implementation and
decode it using recisdb-rs and a legitimately provisioned B-CAS reader. Provide
the px4-userland CLI workflow as closely as the ASICEN hardware permits.

Use `asicend`, `asicenctl`, and `asicen-ts` as the product command names; preserve
px4-userland option names, stdout/stderr separation, tune/channel conventions,
duration/output behavior, and exit-code meanings where supported. Device
identity and the four receiver map are ASICEN-specific and must be documented.
Unsupported operations must fail explicitly; mock success is not hardware proof.

## CLI compatibility target

The read-only px4 comparison found reusable portable IPC, control client/server,
endpoint ownership, tuner/card service, TS client and PC/SC IFD components.
Use those components instead of the former research 24-byte IPC.
Record exact source revisions and GPL-2.0-only notices before incorporating
source. recisdb remains an external downstream program.

- Match `--runtime-dir`, `--instance`, `--receiver`, `--system`, `--channel`,
  `--frequency-khz`, `--slot`, `--stream-id`, `--timeout-ms`, `--packet-count`,
  `--duration-seconds`, and `--output PATH|-` where implemented.
- Runtime devices have no USB serial descriptor. `--list-json` must expose
  `serial: null` and an explicit topology identity; do not invent a serial.
  Define the selection grammar and map before wiring runtime selection.
- Preserve the research receiver map 0:S, 1:T, 2:S, 3:T. Document its difference
  from PX-W3U4's receiver map. Confirm runtime-function ordering before using
  it as a hardware map.
- px4 uses satellite IF kHz; existing ASICEN tables use RF kHz. Their BS/CS
  difference is 10678000 kHz. This is an arithmetic observation, not yet a
  received-satellite validation; retain the px4 input contract if converted.
- Keep TS stdout binary-only and diagnostics on stderr. Use px4 exit meanings:
  2 usage, 3 not found/not ready, 4 busy, 5 timeout, 6 IPC protocol, 7 USB,
  8 integrity/backpressure, 9 card, 10 firmware, 70 internal; 0 success.
- A finite capture must report the authoritative backend terminal result.
  Test slow/disconnected consumers and signal stop; do not infer success merely
  from receiving the requested bytes.
- Preserve research test coverage through the canonical runtime/instance IPC.
  The former `--socket` 24-byte mock protocol is superseded by Issue #4;
  reject its obsolete CLI route explicitly rather than keeping two protocols.
  Mock success must never claim real tuning or card success.

## Shared packet and card contracts (2026-10-13)

Use the exact v0.2.1 `TsContinuityChecker` on complete, transformed 188-byte
packets at the existing HardwareStreamService observation point. Only its
`error` classification increments continuity-errors. One legal same-PID
payload duplicate is accepted; further same-CC packets and byte conflicts are
errors. Adaptation-only packets retain CC, and a valid discontinuity marker
rebases it. Null packets do not affect CC state. TEI and invalid headers retain
the trusted CC baseline and invalidate duplicate adjacency; they do not count
themselves as CC errors. Keep the separate TEI counter. A later trusted packet
can still expose a gap. This is a bounded CC observer, not full MPEG conformance
validation or proof of clean reception.

Allocate one checker before capture outside the callback stack. The released
[upstream checker documentation](https://github.com/Khronos31/px4-userland/blob/dd1c1bea16a5871085d5f2a42c009364c1ec7345/docs/ts-continuity.md)
reports 1,638,408 bytes (about 1.56 MiB) on its tested 64-bit ABI; this is upstream
size evidence, not a measurement of the ASICEN candidate. Allocation failure
returns INTERNAL before source preparation. Reset it at each accepted fresh capture attempt. Preserve all TS
bytes, enqueue/count ordering, source/framer counters, detach/final snapshots,
ASICEN transform, sync probe and native start/stop behavior. Do not import
IT930x startup suppression or treat a lower CC count as a CS2 hardware fix.

Keep IPC version 1 and the existing ASCN identity/topology. The portable card
service now supports opt-in `kCapabilityCardGenerationBinding` (bit 3) and
`CARD_HANDLE_STATUS` (0x0038). The latter requires both CARD and generation
capabilities. Bound handles remain stale after observed card loss until their
owner explicitly reconnects/resets successfully; legacy clients retain the
existing recovery contract. The status operation checks ownership and does not
initialize or rebind a session. These shared service/codec changes do not add
an ASICEN WinSCard adapter or expand hardware-support claims.

## Acceptance increments

### Distribution contract (clarified 2026-10-09)

px4-userland compatibility includes its distribution targets and dependency
contract, not only CLI spelling. This requirement is currently **UNMET**;
successful native development builds and TS trials do not satisfy it.
The current reusable-code parity baseline is official px4-userland v0.2.1
`dd1c1bea16a5871085d5f2a42c009364c1ec7345`. Preserve ASICEN's candidate workflow and
the product-specific distribution matrix below.
The earlier audit used `3de7d512cef11756e6f033557ee49917cad2f11c`.
Windows Phase 1 is now an ASICEN development and build-verification target: the three
product commands and same-host IPC including card operations. Windows real-device
acceptance remains unverified. WinSCard compatibility and Microsoft PC/SC IFD
registration remain Phase 2; a cross-build does not complete hardware acceptance.
The user's 2026-10-11 JST release decision explicitly excludes Windows binaries
from 0.1.0 distribution while retaining the implementation and build checks.
Formal Windows support is planned for 0.2.x after WinSCard support is ready.

- Distribute the three product commands `asicend`, `asicen-ts`, `asicenctl`,
  plus the applicable native card adapter and Android launcher. Research
  diagnostics are not substitutes for product commands or platform artifacts.
- For 0.1.0 produce eight binary archives: Linux x86_64 and aarch64, each with
  glibc and musl IFD variants; macOS arm64; Android aarch64, armv7a and x86_64.
  Also provide one corresponding-source archive and SHA256SUMS. Keep Windows
  x86_64 source, developer packaging support and build tests, but exclude its
  binary archive from the release candidate set and published distribution.
- Linux's three commands must be musl fully static executables, with no
  PT_INTERP, DT_NEEDED or GLIBC version requirements. Merely statically
  linking glibc does not satisfy this contract. Both Linux archive variants
  use the same libc-independent command binaries for their architecture.
- Linux IFD is a shared plugin loaded by host pcscd: provide a glibc 2.31
  compatible variant and a musl variant, as px4-userland does. The command
  binary independence requirement must not be misrepresented as an IFD ABI
  guarantee across different host libcs.
- Statically include libusb wherever the product links it. Clients/IFD that
  use only IPC need not acquire an unnecessary libusb dependency. macOS may
  depend on system libraries/frameworks; Android may depend on API24+ Bionic.
  The pinned reference Windows build also uses static libusb. ASICEN follows
  that current contract and does not package a libusb DLL.
- Package exact dependency licenses/notices, corresponding source and
  reproducible build/relink materials alongside the binaries. Audit the
  final archive inventory, machine architecture and dynamic dependencies;
  run smoke tests from those archives rather than unrelated build trees.
- Keep all targets in the acceptance matrix. An unavailable build/runtime
  or unverified hardware path remains explicitly unverified, never omitted
  or inferred from Linux x86_64 results. The user's later 2026-10-09 instruction
  authorizes public development source before 0.1.0; it does not waive these
  binary-release acceptance requirements.

### Release preparation and publication

`build_userland.yml` remains a candidate-producing workflow: it does not create
tags or GitHub Releases. The separate `release_tag.yml` calls the locally
embedded `release_reusable.yml` from the same ASICEN tag commit. Both helper
checkouts are bound to that exact full commit SHA; an unrelated repository or
mismatched SHA is rejected. No PX4 checkout or cross-repository workflow call
is needed for 0.1.0. The source, license, local adaptations and later migration
to a released PX4 tag's full commit SHA are recorded in
`docs/release-automation-provenance.md`.
It is triggered only by a version tag push after the user's explicit
"tag + release" request. No extra GitHub Environment approval is added.

Complete the required hardware/specification tests and reproducibility check
before writing release notes, and write them only after the user's explicit
request. Commit the user-facing Markdown to `release-notes/vX.Y.Z.md` and the
candidate/run/hash/evidence record to the matching `.json` sidecar. Before the
candidate is frozen, these two files contain only invalid pending placeholders
so the candidate/tag source inventory remains identical. A notes request or
notes commit is not permission to tag or publish.

The shared gate binds two independent successful candidate runs to their exact
source SHA/tree, attempts, artifact identities, complete asset hashes, effective
build inputs and runner images. Require byte-identical candidate bundles and
the exact successful tag build's artifacts. Compare candidate/tag inventories,
file types, modes, link targets and all non-document payload bytes. The existing
ASICEN Linux `evidence/linux-static-build.json` and
`evidence/linux-ifd-build.json` may change only their validated `source_ref`
identity and corresponding manifest hashes/sizes/checksums; every other field
and the referenced executable/library bytes must stay fixed. Firmware bytes
are not a document or metadata exception.

Human evidence must satisfy the existing eight-archive short/LNB matrix,
representative full-channel regression and selected soak requirements in
`docs/release-validation.md`. Machine-readable completion is an attestation,
not proof that a person measured hardware or authorized physical operations.
Keep historical failures and support boundaries. After verifying the Draft's
exact ten assets and committed body, publish Stable/Latest and redownload and
verify again. Stop on missing evidence, incomplete runs or conflicts; never
replace an existing tag or conflicting published asset. Full procedures and
record fields are in `docs/releasing.md` and `docs/release-record.md`.

See [the compatibility audit](docs/compatibility-audit-2026-10-09.md) for the
observed gaps and executable checks. The following reception increments
remain useful development milestones, not a replacement for this contract.

1. Enclosure bring-up: observe two runtime `0b06:0005` functions on the original
   sibling port paths, each with endpoints `81`/`82` and successful read-only
   queries. Establish the GPIO sequence from original-driver evidence before
   attempting sibling restore. Log every setup and response length.
2. Terrestrial frontend: initialize and tune one local ISDB-T lane to an actual
   available physical channel; verify lock from a proven status register.
   Diagnostic verification: `asicen-frontend --device BUS:ADDRESS --port PATH
   --frequency-khz KHZ terrestrial` must finish within its setup/lock deadline
   and report demod register b0 with low nibble 9. On 2026-10-08 primary
   port1-2.1 tuned557142kHz and returned b0=A9: this increment passed on one
   terrestrial lane. Cold-start reproduction and the second lane are unverified.
3. TS: capture at least a short finite sample into ignored local evidence,
   check 188-byte framing, nonzero packet progression, PAT/PMT/CRC, TEI and
   continuity errors. Verify ASICEN transport encryption handling separately
   from ARIB STD-B25 descrambling. Keep raw/error samples for diagnosis.
4. CLI: add daemon/control/TS commands with a written px4 CLI comparison;
   verify argument validation, finite capture, binary-only stdout, busy leases,
   signal/consumer disconnect cleanup, and failure exit codes offline. Preserve
   the existing research test coverage and diagnostic probe entry point.
   Baseline command: `cmake -S . -B build -DASICEN_ENABLE_LIBUSB=ON`,
   `cmake --build build --parallel 2`, and
   `ctest --test-dir build --output-on-failure`. Add meaningful error-boundary
   coverage while preserving or strengthening existing research checks through
   migration to the canonical contracts.
5. Decode: identify a usable PC/SC reader without disrupting existing services;
   run recisdb against the captured sample, require success and verify clear
   elementary-stream decoding. User confirmed terrestrial/satellite antennas
   and B-CAS are connected on
   2026-10-08; reader protocol/operation remains UNVERIFIED. Never substitute
   software test keys for a card.

## Constraints and rollback

- Work only in this repo and `/config/.tools/asicen-work/` for supporting evidence.
  Read px4-userland/recisdb-rs and supplied vendor artifacts as references.
- Do not edit HA config, restart HA/add-ons, change production tuner/card
  services, touch secret files, or release/version-bump.
- Public source development is authorized by the user's 2026-10-09 instruction,
  superseding the initial plan to keep the repository private until 0.1.0.
- Keep additional vendor binaries, TS samples and card data untracked.
  The current repository already tracks the separately identified
  `firmware/asicen-loader.bin`; binary archives include that exact pinned image,
  while corresponding-source exports exclude it. Preserve the provenance and
  unresolved-rights notice in `THIRD_PARTY_NOTICES.md`. Do not label the vendor
  image as covered by the product's open-source license or infer permission
  to add other images. This records the current tree, superseding the older
  outside-the-repository loader description.
- Preserve or strengthen existing test coverage when migrating tests to the
  canonical contracts. Do not weaken expectations, fixtures, mocks, or skips
  merely to make failures pass. Do not adopt kernel modules/legacy ioctl devices.
- LNB electrical testing is deferred until measurement with a physical tester.
  The 2026-10-09 LNB request authorizes implementation and offline regression
  tests; it does not authorize physical power tests. Hardware work, when
  separately requested, targets observed port path/VID/PID and fresh USB
  addresses; never reset the parent hub.
- Stop each hardware increment on unproven or failed setup/status; do not hide
  missing frontend or decryption logic behind mock output.
- Source rollback: preserve starting commit `3b6939a`; work is isolated here.
  Stop only processes created for this task, release handles, retain error
  samples. Cold-loader recovery may need the user's physical power cycle;
  that procedure remains unverified and is not done automatically.

## Prior art gate

Supplied research skeleton: adopt protocol facts and offline models; no real
frontend/backend currently exists. px4-userland: reference/adapt CLI semantics,
ownership and IPC patterns; its IT930x tuner backend is not an ASICEN driver.
recisdb-rs: adopt as the downstream B25 decoder, not as the ASICEN USB transport.
Public prior-art searches on 2026-10-08 identified
[radi-sh/BDASpecial-PlexPX](https://github.com/radi-sh/BDASpecial-PlexPX), a
Windows BDA plugin for ASICEN PLEX receivers. It requires the Windows driver and
BonDriver_BDA, so is a protocol/TS-transform reference, not a direct libusb
replacement. [nns779/px4_drv](https://github.com/nns779/px4_drv) targets later
PX4/MLT hardware and is likewise not a W3U3 ASICEN backend. No complete direct
ASICEN userland replacement was established by these searches. Provenance of
every reused source must be recorded before reuse.

Cheaper alternative: reuse the existing CLI/core components where they fit;
avoid inventing a new end-user workflow. Hidden premises: sibling power/reset,
frontend register programming, ASICEN TS transform and card availability are
not yet established. Runtime enumeration alone does not satisfy TS reception.

## Independent review requirements

The 2026-10-08 independent critic returned REVISE. Before integrating hardware:

- Establish enclosure identity and an ownership lock shared with the probe;
  a second owner must not unlink another daemon's socket or bypass USB ownership.
- Cancel and join all workers before leases/backend/USB handles are destroyed;
  cover partial requests, blocked streams, slow consumers and concurrent clients.
- Use setup/capture deadlines and an authoritative terminal stream result;
  exact byte count must not conceal USB/integrity/output failure.
- Compare reuse of px4 portable IPC/client/lifecycle with extending research
  IPC before selecting an implementation; record provenance and dependencies.
- Keep mock/CLI tests distinct from real station and actual-card evidence.


## 0.1.0 source-supported model boundary

Use [docs/model-support.md](docs/model-support.md) as the current implementation
and evidence matrix for the five USB products. Model recognition, source-backed
runtime support and physical validation are separate states. Unsupported
silicon/controller revisions fail closed; they must not inherit W3U3 writes
merely because the USB vendor is ASICEN. S3U's combined receiver remains one
exclusive resource across terrestrial and satellite tuning. Four-receiver
physical capacity does not imply secondary-function or simultaneous-capture
acceptance. GPIO/LNB electrical testing is deferred to measurement; this does
not prohibit implementing source-backed model-specific control sequences.

LNB requests follow px4-userland's `--lnb-voltage 0|15` ISDB-S interface.
Current software switching is source-backed for W3U2/W3U3/V2 only. S3U/S3U2
physical power support is established, but their recovered setters are no-ops:
`15` must return UNSUPPORTED and `0` must not claim measured power OFF.
See [the LNB contract](docs/lnb-control.md) and its static evidence.
