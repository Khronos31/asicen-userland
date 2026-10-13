# Embedded release automation provenance

## Source and license

For ASICEN 0.1.0, the reviewed PX4 release automation remains embedded as
product-owned local source. Its unchanged shared baseline is now also present
in the official PX4 v0.2.1 release. Two bounded HTTP-boundary corrections below
are maintained locally because that immutable release still contains the
publisher defects. ASICEN does not fetch or execute unreleased PX4 source.
This update does not change the C++ parity baseline or imply hardware
acceptance or release authorization.

- Repository: [Khronos31/px4-userland](https://github.com/Khronos31/px4-userland)
- Original reviewed import: [`b4635226082161ce09f811b1571c9dd25f40cf03`](https://github.com/Khronos31/px4-userland/commit/b4635226082161ce09f811b1571c9dd25f40cf03)
- Upstream work: [PX4 PR #56](https://github.com/Khronos31/px4-userland/pull/56)
- Official released baseline: [PX4 v0.2.1](https://github.com/Khronos31/px4-userland/releases/tag/v0.2.1),
  tag commit [`dd1c1bea16a5871085d5f2a42c009364c1ec7345`](https://github.com/Khronos31/px4-userland/commit/dd1c1bea16a5871085d5f2a42c009364c1ec7345).
  The baseline files listed below have the same bytes as the original import.
- License: GPL-2.0-only. Imported files retain their SPDX notices. The exact
  upstream `LICENSE` is preserved in `third_party/release-automation/LICENSE`.
  This license does not apply to the separately identified vendor firmware.

The original import commit records history. The official tag commit identifies
the released baseline; neither is an active external workflow dependency.
Post-release PX4 fixes are cited below as review evidence for explicit local
changes, never as an external checkout or reusable-workflow pin.

## Import inventory

Hashes below are SHA-256 of upstream file bytes at the official released
baseline (also identical at the original import). `exact` entries remain
byte-identical. The workflow adapter, test fixture selection and the explicitly
listed local HTTP fixes are the only differences. Verifier logic is unchanged.

| Local path | Upstream path | Upstream SHA-256 | Copy |
| --- | --- | --- | --- |
| `scripts/release_verify.py` | `scripts/release_verify.py` | `1c3ebe46b1b7c0d0079c7996a928a7eaefdcec69c01a06e2ff9da143dd34738a` | exact |
| `scripts/release_publish.py` | `scripts/release_publish.py` | `961321ca9a3d8cf4eb762d94961f5edce708a18fbeb49fb9f4126ee8783ab47a` | local HTTP fixes |
| `tests/test_release_verify.py` | `tests/test_release_verify.py` | `1b8e520655b7d8a02390c611c5d739ea0e3088433cba0fe92a1e7f28b7a4941f` | exact |
| `tests/test_release_publish.py` | `tests/test_release_publish.py` | `c01ba5cf866920a7da4f616dfd0d27e8d311336ff40dfc827d4e9b2ffb0276e6` | fixture paths and HTTP regression additions |
| `tests/test_release_integration.py` | `tests/test_release_integration.py` | `00beb76e195bed6450f97d15382a2e4aeef8a3f207fadcd0919b48995b9770f4` | exact |
| `tests/fixtures/release-upstream/release_tag.yml` | `.github/workflows/release_tag.yml` | `55efbf775a55c14604dd344e06f4cfb87d11e43c9aa7880f967bc267d0059f02` | exact |
| `tests/fixtures/release-upstream/release_reusable.yml` | `.github/workflows/release_reusable.yml` | `9e18a7f8f166ef5dcbac14628eb8996c621930dc792cb83d3abd76b411c68c3b` | exact |
| `.github/workflows/release_reusable.yml` | `.github/workflows/release_reusable.yml` | `9e18a7f8f166ef5dcbac14628eb8996c621930dc792cb83d3abd76b411c68c3b` | local adapter |
| `third_party/release-automation/LICENSE` | `LICENSE` | `8177f97513213526df2cf6184d8ff986c675afb514d4e68a404010521b880643` | exact |

## Necessary local adaptations and comparison

- The tag wrapper calls `./.github/workflows/release_reusable.yml`, which GitHub
  resolves from the caller's own commit, and passes `github.sha` as verifier ref.
- The local reusable workflow accepts only ASICEN's product and repository on
  a non-deleted version-tag push. Its explicit trusted-local bootstrap rejects
  all other repositories and any verifier ref that is not the exact 40-character
  tag commit SHA. It does not look for PX4 in `referenced_workflows` because no
  external reusable workflow is called. Both helper checkouts use the ASICEN
  repository at that exact SHA and verify the resulting Git HEAD. Publication
  repeats the SHA guard and all shared verification before any release write.
- The offline test workflow checks out the exact ASICEN commit under test.
  Changes to local scripts, tests and workflow/provenance documents trigger it.
- The imported publisher suite selects frozen PX4 wrapper/reusable workflow
  fixtures only for the upstream workflow/bootstrap tests. All 83 original test
  cases and assertions remain, supplemented by four HTTP-boundary regressions;
  its build-workflow assertions still inspect ASICEN's real build workflow.
  The two other suites are exact copies. `tests/test_release_local.py` cases
  inspect and execute the actual ASICEN workflow guards, enforce local SHA
  binding and reject external fetches. The HTTP suite is also run against the
  ASICEN product profile with mocked openers and no network.
- The fixtures are test data, outside `.github/workflows`; they cannot launch
  a workflow or fetch PX4. Their bootstrap network boundary remains mocked.
  The shared tests continue covering both product profiles even though this
  repository's real publication workflow only accepts ASICEN.

Candidate/reproduction/tag payload checks, fail-closed publication, permissions,
Draft and public redownload checks are unchanged. ASICEN's eight binary archives,
Windows 0.1.0 distribution exclusion, firmware binary/source rules and unresolved
rights notice, fixed hardware/soak requirements and explicit user decisions are
preserved. Offline tests do not certify physical hardware or publish a release.

## Bounded local publisher corrections

The released v0.2.1 publisher has two HTTP-boundary defects also present in
the original ASICEN import. Before freezing a new ASICEN candidate, the local
`scripts/release_publish.py` receives only these corrections:

1. Allow GitHub's canonical compare path with two lowercase 40-hex commit
   SHAs separated by `...` under the selected product repository. Continue
   rejecting traversal, encoded paths, foreign repositories, absolute URLs,
   fragments and control characters. The general `..` rejection must not
   reject this exact GitHub compare endpoint.
2. Use `application/vnd.github+json` on the authenticated, exact
   `/actions/artifacts/<positive-id>/zip` API request. Use
   `application/octet-stream` for Release assets and the subsequent signed
   artifact download. Redirects remain HTTPS/approved-host only, and the token
   is never forwarded to the signed download host.

Review evidence: [PX4 PR #61](https://github.com/Khronos31/px4-userland/pull/61)
for the compare-path correction and [PX4 PR #62](https://github.com/Khronos31/px4-userland/pull/62)
for the Actions media contract. These references document local corrections;
they are not dependency pins. No recovery-only Publisher options,
`release_recover.py`, hardcoded PX4 v0.2.1 data, recovery workflow or recovery
tests are imported.

The regression tests exercise the real `GitHub.api_url`, `request` and
`download` code through mocked HTTP openers rather than bypassing that boundary
with `FakeAPI`. They check canonical/hostile compare paths, the Actions
415/302 media contract, binary Release downloads and token-free redirect
handling. This is offline boundary coverage, not a claim of live publication
or physical-hardware validation.

## Later replacement

PX4 v0.2.1 officially contains the shared automation, but its publisher still
has the two defects above. Keep ASICEN's existing local binding for this
candidate. An eventual external replacement must use an official release that
contains the required corrections: verify the tag's full commit SHA, replace
the workflow and helper reference together, retain immutable pin binding, and
rerun both product suites plus the caller's security tests. The released
reusable workflow always checks out its own PX4 SHA; a patched ASICEN helper
cannot silently override it. Do not use a floating tag/branch or an unreleased
external commit.

This replacement is a code/workflow change and must happen before freezing a
new candidate and performing the required validation. Do not slip it into the
documentation-only commits between a validated candidate and its release tag.
ASICEN 0.1.0 proceeds with the local copy and does not wait for that replacement.
