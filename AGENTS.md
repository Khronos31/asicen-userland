# AGENTS.md

## ASICEN parity invariant

- ASICEN is the ASICEN hardware variant of px4-userland, not a separately redesigned product.
- Align all non-hardware processing with the pinned PX4 reference: APIs, lifecycle, errors, CLI, IPC, paths, environment variables, control-flow syntax, build scripts, workflows and packaging.
- Retain only the smallest evidence-backed hardware, topology, firmware and product-identity differences. An unmatched filename or passing local test is not a hardware exemption.
- Update the comparison record whenever changing a shared contract. Do not freeze a divergent behavior into tests merely because it is the current ASICEN implementation.

## Source of truth

- Normative product requirements are in `SPEC.md`. Read the relevant section before implementation.
- If an observed device behavior conflicts with the frozen specification, update its version and rationale before changing the implementation.
- Supported device profiles and their verification status are defined by the current `SPEC.md` and `README.md`. Do not claim support for another related device without a versioned specification change and the profile-specific hardware evidence required by `SPEC.md`.

## Supported scope

- Runtime targets are Linux (including environments where kernel modules cannot be installed), Android/Termux, macOS, and Windows 11 x64 (Phase 1).
- Every target runtime must support both the tuners and its internal card reader.
- Windows Phase 1 provides `asicend`/`asicen-ts`/`asicenctl` and the versioned local IPC (including `CARD_*`) for same-host consumers. It uses the same wire protocol and `control.sock`/`stream.sock` concept. WinSCard DLL compatibility and Microsoft PC/SC IFD registration are Phase 2 and out of scope now. `tsukumijima/px4_drv` remains a separate product.
- Windows hardware claims stay `hardware-unverified` until real-device evidence exists; build and offline test success is only `build-tested`.
- The user's 2026-10-11 JST release decision keeps Windows source and build verification in 0.1.0 but excludes Windows binaries from distribution. Formal Windows support is planned for 0.2.x when WinSCard is ready. Do not infer a release/version bump from the presence of Windows code.
- Android ad-hoc APK hardware testing is owned by dtv-android and is outside this repository's release gates and support claims. The APK is not a release artifact.
- FreeBSD is outside the product scope; validation-results.md entries for it are historical only.
- ASICEN loader firmware is a separate vendor component. Preserve the exact binary/source archive policy and unresolved rights notice in `THIRD_PARTY_NOTICES.md`; do not infer a GPL grant from module metadata.

## Implementation rules

- Keep the portable core C++17, exception-free and RTTI-free. Do not use glibc extensions or GNU-only APIs required for core functionality.
- Preserve glibc, musl, Bionic API 24+, macOS, and Windows x64 portability. Keep platform APIs outside the portable core; Windows-specific code lives in dedicated files under `userland/src/windows/` and behind `_WIN32`, leaving the existing POSIX code in `#else` unchanged.
- Use fixed-width integers and checked lengths at USB, firmware, IPC, ATR, APDU, and TS boundaries.
- Preserve existing tests. Do not change expectations, fixtures, mocks, or skips merely to make a failure pass; add tests for new behavior.
- Do not reintroduce Linux kernel modules, chardev/ioctl interfaces, DKMS/Debian packaging, legacy udev rules, legacy Windows-only host implementations, or implementations for devices outside the supported models. Windows Phase 1 code is libusb-based and does not add WinUSB INF or kernel components.
- Physical USB changes, card insertion/removal, antenna changes, power changes, and other hardware operations require user confirmation before execution.
- Preserve existing dirty-tree work. Use `apply_patch` for edits and do not reset, checkout, or broadly reformat unrelated files.
- Do not run `git add`, `git commit`, or `git push` unless the user explicitly requests that operation.
- Create public issues only for unresolved problems known at publication time; do not create preventive placeholder issues.

## Release notes

- GitHub Release のタイトルは `vX.Y.Z` のみとし、先頭に製品名などを付けない。
- 本文の先頭見出しは `asicen-userland vX.Y.Z` とする。
- 概要は敬体（です・ます調）で簡潔に書く。
- 「主な変更」「検証」「既知の制限」は箇条書きの常体で書く。該当項目がない節は省略し、埋め草を入れない。
- 「謝辞」は敬体で書く。
- リリースノートは利用者向けの変更概要と必要な注意に絞る。内部監査記録、詳細な試験ログ、ハッシュ一覧、余計な検証 matrix は載せない。環境別の詳細が必要な場合は README 等の正本へリンクし、matrix を複製しない。

```markdown
# asicen-userland vX.Y.Z

[概要を敬体で簡潔に記載]

## 主な変更
- [変更点を常体で記載]

## 検証
- [検証結果を常体で簡潔に記載]

## 既知の制限
- [必要な場合のみ、常体で記載]

## 謝辞
- [貢献者への謝意を敬体で記載]
```

## Stable release validation

- Follow ASICEN's `SPEC.md`, `docs/release-validation.md` and recorded hardware acceptance conditions. The parity update does not replace earlier release evidence or certify new binaries.
- Preserve recorded user choices about soak duration and platforms. Do not import PX4-specific physical-action deadlines, notification commands or section numbers as new ASICEN requirements.
- Build/offline test success is not hardware verification. Keep changed candidate binaries unverified until their required real-device evidence exists.
- Physical USB/card/antenna/power operations require user confirmation. Do not perform them for a static parity or offline regression task.
- The user decides any new soak duration or hardware test scope; provide the affected behavior and evidence needed instead of inventing a release gate.

## Handoff requirements

- Report changed files, commands run, results, and unverified scope at the end of each increment.
