# ASICEN userland development contract

Status: acceptance plan, updated 2026-10-10 for Issue #4 parity against
px4-userland `1a1485d0c3e972e0a47be907edb67949564aa9a7`.
This is not a release/support claim.

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

## Acceptance increments

### Distribution contract (clarified 2026-10-09)

px4-userland compatibility includes its distribution targets and dependency
contract, not only CLI spelling. This requirement is currently **UNMET**;
successful native development builds and TS trials do not satisfy it.
The current parity baseline is px4-userland
`1a1485d0c3e972e0a47be907edb67949564aa9a7` and its release-candidate workflow.
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
   the existing research tests and diagnostic probe entry point.
   Baseline command: `cmake -S . -B build -DASICEN_ENABLE_LIBUSB=ON`,
   `cmake --build build --parallel 2`, and
   `ctest --test-dir build --output-on-failure`. Add meaningful error-boundary
   coverage without modifying existing research tests.
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
- Do not change existing tests or adopt kernel modules/legacy ioctl devices.
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
