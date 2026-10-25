# 安定版リリース前検証手順

この文書はASICEN Stable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように版別文書へ記録します。公開契約は [`SPEC.md`](../SPEC.md)、CLI構文・機種能力は [`README.md`](../README.md) と各コマンドの `--help` が正です。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは一定時間以上の連続したTS取得を指す。時間条件は下の適用頻度と版別matrixに従う。製品・機種ごとの能力、対応機能、実コマンドは続く製品固有節と手順節に記し、版別記録にはcandidateごとの実施結果を記す。

| 区分 | 適用頻度 | 共通の確認 |
|---|---|---|
| 機種初回 | 各製品の全対象機種で、機種ごとに代表機の2時間連続受信と毎release項目を実施する。必要時項目も通し、結果は機種別に初回記録へ残す。 | 当該機種の公開能力・対応機能に沿って実行し、別機種の結果で代替しない。 |
| 回帰（毎release） | 代表機で2時間連続受信する。全配布archiveをそれぞれ対応するnative OS/ABIで短時間実機回帰し、`--list-json`、機種の公開同時受信能力、TS/counter/終了値を確認する。最大同時受信で地上波・衛星の対象3波を確認できない場合は不足波を短時間取得する。受信中の状態と反復制御、正常停止、process/FD/IPC残留なし、対応機能のCARD挿抜、USB抜去/再挿入を確認する。native PC/SC/WinSCard経路がある製品・OSでは製品CLI経路と分けて確認する。 | 各archive/OS/ABIのrunと各機種の結果を分ける。未対応機能は理由付きでN/Aとし、未実施や失敗を合格に読み替えない。 |
| 必要に応じて | 対応機能ではLNB出力電圧を実測し、非対応機種の拒否を確認する。複数チューナーの列挙/daemon並列と、変更機能に応じた機種×OS/ABIの追加確認も行う。 | 適用する機種・機能・OS/ABIと実測値を記録する。 |
| 初回/major | 必要時項目を通し、代表機で全対象の無料channelを各10秒TS取得する。ソークはmacOS/Windows各30分、残るOS/ABI各10分とする。全channel取得は代表機だけで行い、別途指示されたソークは別に記録する。 | 30分/10分は各OS/ABIの個別時間であり、共通の一般定義である「一定時間以上の連続TS取得」を置き換えない。 |

全配布archiveのnative短時間回帰と代表機だけの全channel取得は別の確認である。候補の操作・期待値・cleanupは各archiveの具体手順に従い、実施状態・結果・未達は版別記録に残す。

## 製品固有の適用条件

ASICENの通常の無指定時検証機種はPX-W3U3だが、機種初回の対象を限定しない。ASICENが対象とする全機種で初回手順を個別に実施し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ機種別に記録する。

| 区分 | 製品固有の確認 |
|---|---|
| 機種初回 | PX-W3U2とPX-W3U3は4番組同時録画、地上波/衛星、CARD、対応LNBを確認する。ほかの機種はREADME記載の機種別能力に従う。 |
| 回帰（毎release） | PX-W3U2とPX-W3U3は4番組を同時録画し、各TS/counter/終了値を確認する。W3U3のtuner path数、discovery route数、公開slot数は録画数と別々に記録する。WinSCard/PCSCはnative stackを別経路で確認する。 |
| 必要に応じて | W3U3はLNB対応。W3U2/W3U3/V2以外はREADMEの能力表で確認し、非対応機種へsoftware要求を送らない。 |
| 初回/major | 現地channel planとmodel receiver割当を記録。Windows ZIPは通常aggregate外でも同候補の`win-candidate`を使う。 |

## 共通実行環境表

表の列と値の意味は4製品で共通とする。ここでは候補の対象OS/ABIと実行前提を記し、host名、candidateの実施状態、過去実績は版別文書へ記す。

### 環境表データ辞書

| 列 | 意味・含める属性 | 含めない属性 | 表記・許容値 |
|---|---|---|---|
| ID | OS/ABIと必要なruntime/access境界を持つ実行環境シナリオの識別子 | candidate状態、過去実績 | `E` + 番号。4製品で共通するシナリオは同じID、別runtime/access scenarioは別ID |
| OS / ABI | OS系統、CPU ABI、libcまたはOS runtime | host名、依存、実施状態 | `OS / CPU ABI / libc・runtime` の順。根拠文書にlibc記載がない場合は `未特定` |
| 依存 / driver / permission | 実行時USB library、OS側USB access手段と権限条件 | firmware、CARD API、検証結果 | `libusb:`、`USB access:`、`FD handoff:` の属性名を使う |
| firmware / 補助data | 機器へ渡す実行時firmwareと、RF channelとは別の機種固有データ | RF試験channel、archiveへの同梱・埋込完了、candidate実績 | `firmware:` と `補助data:` を分け、ない属性は `なし` |
| RF試験入力（system / channel） | 実際に受信させる放送systemと現地channel | firmware、機種data、candidate実績 | `system:` と `channel:`。対応波がない場合は `なし（理由）` |
| 製品CLIからの内蔵CARD操作 | 製品固有CLI/APIの操作範囲 | PC/SC/WinSCard adapter、実機合否 | 操作名とmodel依存条件。機能なしは `なし` |
| native PC/SC/WinSCard adapter | その行のOS標準APIへ接続する製品adapterの種類・配布形態 | 製品CLI CARD、adapter実機試験結果、他OSのadapter | OS/ABI行に該当する`Linux PC/SC:`、`macOS PC/SC:`、`Android:`または`Windows WinSCard:`だけを記載 |
| 配布artifact / 手順 | 対象archive識別子とこの正本内の該当手順 | artifact hash、host、試験結果 | `artifact: <target>; procedure: [archive別手順](#3-配布アーカイブ別手順)` の順 |

共通IDは E02=Linux/x86_64/musl、E03=Linux/x86_64/glibc、E04=macOS/arm64、E05=Android/aarch64、E06=Android/armv7a、E07=Android/x86_64、E15-G=Linux/aarch64/glibc、E15-M=Linux/aarch64/musl、E17=Windows 11/x86_64を表す。IDはhost名や試験済み状態を表さない。

| ID | OS / ABI | 依存 / driver / permission | firmware / 補助data | RF試験入力（system / channel） | 製品CLIからの内蔵CARD操作 | native PC/SC/WinSCard adapter | 配布artifact / 手順 |
|---|---|---|---|---|---|---|---|
| E03 | Linux / x86_64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-glibc-x86_64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E02 | Linux / x86_64 / musl | libusb: 静的1.0.30; USB access: HAOS Supervisorの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-musl-x86_64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E15-G | Linux / aarch64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-glibc-aarch64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E15-M | Linux / aarch64 / musl | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-musl-aarch64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E04 | macOS / arm64 / Apple system runtime | libusb: 静的1.0.30; USB access: macOSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | macOS PC/SC: bundle | artifact: `darwin-arm64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E05 | Android (Termux) / aarch64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; FD handoff: daemonへ2 FD | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Android: なし | artifact: `android-aarch64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E06 | Android (Termux) / armv7a / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; FD handoff: daemonへ2 FD | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Android: なし | artifact: `android-armv7a`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E07 | Android (Termux) / x86_64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; FD handoff: daemonへ2 FD | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Android: なし | artifact: `android-x86_64`; procedure: [archive別手順](#3-配布アーカイブ別手順) |
| E17 | Windows 11 / x86_64 / Win32 | libusb: 静的1.0.30; USB access: 既存許可済みlibusb driver | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Windows WinSCard: application-local DLL | artifact: `win-candidate`; procedure: [archive別手順](#3-配布アーカイブ別手順) |

## 共通試験シナリオ T00–T14

### run別candidateの取得と作業directory

この手順書commitとcandidate source SHAは一致不要。成功した`build_userland.yml` runについてhead SHA、attempt、結論、artifact ID/name/expiryを照合し、該当版matrixのtarget archiveだけを使う。手動dispatchや正式Releaseは実行しない。ASICEN firmwareの供給/同梱は別途確定したrepo配布物が用意されるまで未達として止め、候補取得だけで解決扱いしない。

```sh
set -eu
gh run list --repo Khronos31/asicen-userland --workflow build_userland.yml --limit 20
RUN_ID='<successful-candidate-run-id>'; TARGET='<linux-glibc-x86_64|linux-musl-x86_64|linux-glibc-aarch64|linux-musl-aarch64|darwin-arm64|android-aarch64|android-armv7a|android-x86_64>'
RUN_JSON=$(gh run view "$RUN_ID" --repo Khronos31/asicen-userland --json headSha,attempt,conclusion)
SOURCE_SHA=$(printf '%s\n' "$RUN_JSON" | python3 -c 'import json,sys; print(json.load(sys.stdin)["headSha"])')
printf '%s\n' "$RUN_JSON" | python3 -c 'import json,sys; r=json.load(sys.stdin); assert r["conclusion"] == "success" and r["headSha"] and r["attempt"] > 0'
test -n "$SOURCE_SHA"
gh api "repos/Khronos31/asicen-userland/actions/runs/$RUN_ID/artifacts" --jq '.artifacts[] | [.id,.name,.expired] | @tsv'
WORK=$(mktemp -d "${TMPDIR:-$HOME}/asicen-run-${RUN_ID}.XXXXXX"); chmod 700 "$WORK"; mkdir -m 700 "$WORK/download"
gh run download "$RUN_ID" --repo Khronos31/asicen-userland --name release-candidate --dir "$WORK/download"
cd "$WORK/download"; sha256sum -c SHA256SUMS || shasum -a 256 -c SHA256SUMS
VERSION='<candidate-version>'; ARCHIVE="asicen-userland-$VERSION-$TARGET.tar.gz"
mkdir "$WORK/candidate"; tar -xzf "$WORK/download/$ARCHIVE" -C "$WORK/candidate"
D="$WORK/candidate"; LOG="$WORK/log"; RT="$WORK/runtime"; mkdir -m 700 "$LOG" "$RT"; cd "$D"; export PATH="$D:$PATH"
test -x "$D/asicend"; test -x "$D/asicenctl"; test -x "$D/asicen-ts"
python3 -c 'import json,sys; m=json.load(open("manifest.json")); assert m.get("source_ref")==sys.argv[1] and m.get("platform")==sys.argv[2]' "$SOURCE_SHA" "$TARGET"
```

Windows対象版は同runの`win-candidate` artifactを別に取得し、`Get-FileHash -Algorithm SHA256`でZIPを照合して空のrun固有directoryへ展開する。通常`release-candidate`にはWindows ZIPがないので、欠落を通常archiveとして補わない。Linux/macOS/Termuxでは`gh`, `python3` (Termux: `pkg install gh python coreutils tar`), SHA-256 tool, `tar`を確認し、Windowsでは`gh`と`python --version`を確認する。追加installはhost管理者承認後に行う。

```powershell
$ErrorActionPreference = 'Stop'
$RunId = '<successful-candidate-run-id>'; $Version = '<candidate-version>'; $CandidateSourceSha = '<candidate-source-sha>'
$Work = Join-Path $HOME "asicen-win-$RunId"; $Download = Join-Path $Work 'download'; $D = Join-Path $Work 'candidate'
if (Test-Path $Work) { throw "refusing to reuse $Work" }
New-Item -ItemType Directory -Path $Download,$D,(Join-Path $Work 'log'),(Join-Path $Work 'runtime') | Out-Null
gh run view $RunId --repo Khronos31/asicen-userland --json headSha,attempt,conclusion
gh api "repos/Khronos31/asicen-userland/actions/runs/$RunId/artifacts" --jq '.artifacts[] | [.id,.name,.expired] | @tsv'
gh run download $RunId --repo Khronos31/asicen-userland --name win-candidate --dir $Download
$ZIP = Join-Path $Download "asicen-userland-$Version-windows-x86_64.zip"
if (-not (Test-Path $ZIP)) { throw "candidate ZIP missing: $ZIP" }
$jobs = (gh run view $RunId --repo Khronos31/asicen-userland --json jobs | ConvertFrom-Json).jobs
$winJob = $jobs | Where-Object { $_.name -eq 'Windows x64 development cross-build, PE audit, and package regression' }
if (-not $winJob) { throw 'Windows candidate build job missing' }
$runLog = gh run view $RunId --repo Khronos31/asicen-userland --job $winJob.databaseId --log
$hashLine = $runLog | Select-String -Pattern 'windows zip sha256: ([0-9a-f]{64})' | Select-Object -Last 1
if (-not $hashLine) { throw 'Windows archive SHA-256 missing from candidate build log' }
$expectedFromRunLog = $hashLine.Matches[0].Groups[1].Value
$actual = (Get-FileHash -Algorithm SHA256 $ZIP).Hash.ToLowerInvariant()
if ($actual -ne $expectedFromRunLog) { throw 'Windows candidate archive hash mismatch' }
Expand-Archive -LiteralPath $ZIP -DestinationPath $D
if (-not (Test-Path (Join-Path $D 'manifest.json'))) { throw 'candidate manifest missing' }
$manifest = Get-Content (Join-Path $D 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.source_ref -ne $CandidateSourceSha) { throw 'candidate source commit mismatch' }
$LOG = Join-Path $Work 'log'; $RT = Join-Path $Work 'runtime'
```

job名/ID・head SHAとsource refが当該candidate一致しない場合、またはhash行がない場合は未達で停止する。firmware imageの取得・同梱は本レシピの対象外であり、別途確定した供給手順がない限りUSB試験へ進まない。

`WORK`を作るOSでは`LOG`/`RT`もrun固有directoryとして先に生成する。Termuxは`$PREFIX/tmp`または`$HOME`配下を使い、OS `/tmp`を前提にしない。`termux-usb -l`で対象VID/PID/product nameを照合したnodeだけを`termux-usb -r <path>`へ渡す。権限不足はLinux `id`/`stat`/`lsusb -t`、Termux `termux-usb -l`で読み取り記録して停止し、udev/権限の変更は別途承認、変更前保存、試験後復元を記録する。HAOS/Supervisor stateをこの手順から変更しない。

各IDを同じ順序・同じ6項目で実施する。$RT/$LOG/$BUS-PORT/$CHANNEL_PLANはrunごとの実値に置換し、空のcandidate専用runtime/logを使う。物理操作を依頼する各回は既定通知を行い、最大5分で応答がなければ未完了として停止する。OS/API固有の長い操作は同じT-IDから後続recipeを参照する。

### T00 候補/配布物固定

- **適用対象/頻度:** 毎releaseのcandidate作成時。
- **準備:** source SHA、CI run/attempt、archive targetとhashを固定。POSIXではgit rev-parse HEADを保存し、macOSはshasum -a 256、Linuxはsha256sum、WindowsはGet-FileHashでhashを確認する。
- **実操作/利用者への依頼:** 固定したsource commitとCI runから候補archiveを取得し、macOSでは`shasum -a 256 -c SHA256SUMS`、Linuxでは`sha256sum -c SHA256SUMS`、WindowsではGet-FileHashでarchive hashを照合する。対象archive個別のdigestもmacOSでは`shasum -a 256 "$ARCHIVE"`、Linuxでは`sha256sum "$ARCHIVE"`で表示し、manifest entryと一致することを確認する。archiveのmanifest/source refがcandidate commit/runと一致することを確認する。
- **期待値/終了値:** provenance/hash一致の候補だけ次へ進む。候補不在は未完了。
- **停止/cleanup:** deviceを開かず、archive/logを保持。
- **記録:** source SHA、CI run/attempt、artifact ID/target/hash、取得日時、未取得理由。

### T01 実行環境・アクセス権・入力

- **適用対象/頻度:** 全native archive。初回は全modelごと。
- **準備:** E-ID/OS/ABI、USB access、firmware/model data、channel planを環境表と照合。
- **実操作/利用者への依頼:** Linux uname -a/uname -m/ldd --version、macOS sw_vers/uname -m、Android getprop/uname -m、Windows PowerShell OS/architectureを記録。
- **期待値/終了値:** OS/ABIがarchiveに一致し、アクセス許可/入力が揃う。不一致はdevice操作前に止める。
- **停止/cleanup:** 権限不足やdriver変更が必要なら操作せず未実施記録。
- **記録:** E-ID/host/OS/ABI/model、USB topology/permission、firmware/data、channel plan。

### T02 列挙・selector・ready確認

- **適用対象/頻度:** 毎release、対象model/device。
- **準備:** 候補archiveのlist-json/models入口とdevice selectorを準備しdaemon専用runtimeを作る。
- **実操作/利用者への依頼:** asicend --list-jsonを保存し、modelとBUS-PORTを記録する。asicend --device usb:<BUS-PORT[,BUS-PORT]> --instance rc --firmware <FIRMWARE> --runtime-dir "$RT"を起動し、asicenctl --instance rc --runtime-dir "$RT" statusとlistを実行する。
- **期待値/終了値:** JSON/portが一意、status ready、receiver数が公開能力と一致。不正selectorは成功でない。
- **停止/cleanup:** daemon通常停止しprocess/FD/IPC/socket残留を確認。
- **記録:** JSON、selector、status/list、各exit、residual。

### T03 最大同時受信

- **適用対象/頻度:** 毎release、各modelの公開最大能力。
- **準備:** 各公開receiver/deviceを一意に割り当て、各clientに別TS出力/ログを用意する。`CAPTURE_SECONDS`には正の有限秒数（例: `10`）を明示設定して記録する。
- **実操作/利用者への依頼:** asicenctl --instance rc --runtime-dir "$RT" listで一意なR1–R4を選ぶ。次の4 clientを同時起動し各PIDを個別waitする。衛星channel/receiverはREADMEと現地割当へ置換する。

    D='<extracted-candidate-dir>'; LOG='<new-private-run-log-dir>'; RT='<new-private-run-runtime-dir>'
    mkdir -m 700 "$LOG" "$RT"; export PATH="$D:$PATH"; test -x "$D/asicen-ts"
    R1='<listed-terrestrial-receiver-1>'; R2='<listed-terrestrial-receiver-2>'; R3='<listed-satellite-receiver-1>'; R4='<listed-satellite-receiver-2>'
    CAPTURE_SECONDS=10
    test -n "$R1" && test -n "$R2" && test -n "$R3" && test -n "$R4" && test "$CAPTURE_SECONDS" -gt 0
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R1" --channel T27 --time "$CAPTURE_SECONDS" --output "$LOG/terrestrial-1.ts" & P1=$!
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R2" --channel T30 --time "$CAPTURE_SECONDS" --output "$LOG/terrestrial-2.ts" & P2=$!
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R3" --channel BS01_0 --lnb-voltage 0 --time "$CAPTURE_SECONDS" --output "$LOG/satellite-1.ts" & P3=$!
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R4" --channel BS15_0 --lnb-voltage 0 --time "$CAPTURE_SECONDS" --output "$LOG/satellite-2.ts" & P4=$!
    for P in "$P1" "$P2" "$P3" "$P4"; do if wait "$P"; then RC=0; else RC=$?; fi; printf "%s %s\n" "$P" "$RC"; done
- **期待値/終了値:** 各clientの稼働時間が重なり、各exit 0とTS/counter合格。順次取得を同時試験の代用にしない。
- **停止/cleanup:** 全client通常終了後daemon通常停止、残留なし。
- **記録:** model/receiver/system/channel、各start/end/exit、TS/counter、cleanup。

### T04 不足波・全無料channel

- **適用対象/頻度:** 毎releaseは不足波。初回/majorは全機種の代表機で全無料channel各10秒。
- **準備:** 現地channel planとmodel能力から対象system/channel一覧を作る。
- **実操作/利用者への依頼:** asicen-ts --instance rc --runtime-dir "$RT" --receiver <ID> --channel <CHANNEL> --time 10 --output "$LOG/<CHANNEL>.ts"を各channelで実行。
- **期待値/終了値:** 各対象channelのTS/exit合格。未取得は未完了、非対応波だけ根拠付きN/A。
- **停止/cleanup:** client wait/exit保存、daemon通常停止。
- **記録:** model/system/channel/全channel list、TS解析/counter/exit、未取得理由。

### T05 受信中の状態確認・制御

- **適用対象/頻度:** 毎release。CARD/controlは製品能力に従う。
- **準備:** capture clientを起動し、別terminalでstatus/list/controlを行えるようにする。
- **実操作/利用者への依頼:** 受信中に別terminalからasicenctl --instance rc --runtime-dir "$RT" statusとlistを実行し、対応modelでは同CLIのcard-apdu 90:30:00:00:00を10回個別送信して各終了値を保存する。
- **期待値/終了値:** 照会/control時刻がstreamと対応し各response/exitを保存。機能外操作はN/A。
- **停止/cleanup:** client/daemonを通常停止し残留確認。
- **記録:** 照会/control時刻、response class、各exit、streamとの重なり。

### T06 CARD物理挿抜・復帰

- **適用対象/頻度:** CARD reader/status対応model。毎release。
- **準備:** capture停止・daemon維持。初期CARD状態とreader有無を確認。
- **実操作/利用者への依頼:** asicenctl card-status/card-atr/card-reset/card-apduの初期値を採取。抜去依頼/最大5分待ち後同CLI、再挿入を別依頼して再照会。
- **期待値/終了値:** 抜去後statusはabsent、ATR/APDUはCARD_REMOVEDまたはNO_CARD (exit 9)。再挿入後status/ATR/reset/APDUが復帰し、各通常操作はexit 0。CARD対応readerがないmodelは理由付きN/A。応答なしは未完了。raw ATR等は保存しない。
- **停止/cleanup:** daemon通常停止、FD/IPC/runtimeを確認。
- **記録:** 依頼時刻、状態/response/exit、復帰、cleanup。

### T07 USB待機中・受信中の切断と再取得

- **適用対象/頻度:** 毎release。USB対応deviceのidle/streamを別run。
- **準備:** USB pathをlist-jsonで固定。有限captureとそれより短い観測窓を準備。
- **実操作/利用者への依頼:** `asicend --list-json`でUSB pathを特定し、streamなしのdaemonを起動してstatus/listとdaemonのUSB owner状態を記録する。daemonがdeviceを保持したidle中に利用者へ抜去を依頼し、応答後にstatus/list、診断、daemonの実際の終了値を採取する。通常停止後、再接続を依頼し、再列挙したpathで新runtime/daemonを起動してstatus/listと短い`asicen-ts`を実行する。別runでは有限`asicen-ts`中に抜去を依頼し、client/daemonの切断診断と終了値を採取する。
- **期待値/終了値:** idle抜去では保持中USBの切断がdaemon状態/診断に現れ、実際に終了したprocessの値をそのまま記録する。stream抜去では切断診断とclient exit 7を確認する。いずれもtime満了exit 0は切断合格でない。新daemonの短TSはexit 0とTS/counter合格。
- **停止/cleanup:** idle/streamそれぞれでowned client/daemonを通常停止してwaitし、再接続後の新daemonも通常停止する。Androidはpermission/FD再取得。
- **記録:** idle/stream、daemon USB owner状態、依頼/応答時刻、path、status/診断、各実終了値、再取得TS/counter、cleanup。

Linux native Bash runで使うidle/stream手順。各物理操作は利用者へ依頼しEnter応答を最大300秒待ち、daemon/client PIDと返却値を保存する。再接続後は必ず新しいruntimeを使う。

```sh
PORT='<listed-usb-port>'; TOKEN='idle'; ID="usb:$PORT"
RT_IDLE="$WORK/runtime-idle"; mkdir -m 700 "$RT_IDLE"
"$D/asicend" --device "$ID" --instance "$TOKEN" --firmware "$FW" --runtime-dir "$RT_IDLE" > "$LOG/idle-daemon.log" 2>&1 & IDLE_PID=$!
printf '%s\n' "$IDLE_PID" > "$LOG/idle-daemon.pid"
"$D/asicenctl" --instance "$TOKEN" --runtime-dir "$RT_IDLE" status > "$LOG/idle-before.txt" 2>&1
"$D/asicenctl" --instance "$TOKEN" --runtime-dir "$RT_IDLE" list > "$LOG/idle-list-before.txt" 2>&1
printf '%s' 'USB抜去完了後にEnter (300秒以内): '
if ! IFS= read -r -t 300 _; then printf '%s\n' timeout > "$LOG/idle-human-wait.txt"; exit 1; fi
# 操作後の状態と実終了値を記録する。
"$D/asicenctl" --instance "$TOKEN" --runtime-dir "$RT_IDLE" status > "$LOG/idle-removed.txt" 2>&1 || :
kill -INT "$IDLE_PID"; if wait "$IDLE_PID"; then IDLE_RC=0; else IDLE_RC=$?; fi
printf '%s\n' "$IDLE_RC" > "$LOG/idle-daemon.rc"
printf '%s' 'USB再接続完了後にEnter (300秒以内): '
if ! IFS= read -r -t 300 _; then printf '%s\n' timeout > "$LOG/reconnect-human-wait.txt"; exit 1; fi
# 再列挙で新しいPORTを選んでから続ける。
"$D/asicend" --list-json > "$LOG/discovery-reconnected.json"
PORT='<new-listed-usb-port>'; ID="usb:$PORT"; TOKEN='stream'; RT_STREAM="$WORK/runtime-stream"; mkdir -m 700 "$RT_STREAM"
"$D/asicend" --device "$ID" --instance "$TOKEN" --firmware "$FW" --runtime-dir "$RT_STREAM" > "$LOG/stream-daemon.log" 2>&1 & STREAM_PID=$!
printf '%s\n' "$STREAM_PID" > "$LOG/stream-daemon.pid"
"$D/asicenctl" --instance "$TOKEN" --runtime-dir "$RT_STREAM" status > "$LOG/stream-ready.txt" 2>&1
"$D/asicen-ts" --instance "$TOKEN" --runtime-dir "$RT_STREAM" --receiver '<listed-receiver>' --channel T27 --time 300 --output "$LOG/stream-cut.ts" > "$LOG/stream-cut.out" 2> "$LOG/stream-cut.err" & CLIENT_PID=$!
printf '%s\n' "$CLIENT_PID" > "$LOG/stream-client.pid"
printf '%s' 'stream中にUSB抜去完了後Enter (300秒以内): '
if ! IFS= read -r -t 300 _; then printf '%s\n' timeout > "$LOG/stream-human-wait.txt"; exit 1; fi
# 物理切断後、有限観測後にclientをwaitする。
if wait "$CLIENT_PID"; then CLIENT_RC=0; else CLIENT_RC=$?; fi
printf '%s\n' "$CLIENT_RC" > "$LOG/stream-client.rc"
"$D/asicenctl" --instance "$TOKEN" --runtime-dir "$RT_STREAM" status > "$LOG/stream-after-cut.txt" 2>&1 || :
kill -INT "$STREAM_PID"; if wait "$STREAM_PID"; then STREAM_RC=0; else STREAM_RC=$?; fi
printf '%s\n' "$STREAM_RC" > "$LOG/stream-daemon.rc"
```

stream clientの期待値はUSB切断診断とexit 7であり、時間満了exit 0は不合格。idleでdaemonが生存する場合は状態/診断を残してからowned daemonを通常停止する。物理復帰後の新daemonで再列挙・ready・短TS成功を別の新runtimeで記録する。物理操作待ちが上限を超える場合は未完了として停止し、切断・再取得成功に読み替えない。

### T08 通常停止・cleanup

- **適用対象/頻度:** 全test終了時。
- **準備:** 各owned client/daemon PIDとruntimeを記録してから起動する。
- **実操作/利用者への依頼:** clientを通常終了しwait "$P"、daemonは通常SIGINT/console stopしwait。
- **期待値/終了値:** 全owned process終了、TS close、FD/IPC/socket/runtime残留なし。timeoutはfail。
- **停止/cleanup:** 無関係processへsignalしない。test専用runtimeだけ片付け失敗log保持。
- **記録:** PID/role/exit、process/FD/IPC/socket/runtime、復旧状態。

POSIX cleanupでowned PIDの終了値と残留を保存する。

```sh
cleanup_pids="${CLIENT_PID:-} ${CLIENT_B_PID:-} ${DAEMON_PID:-} ${IDLE_PID:-} ${STREAM_PID:-}"
for pid in $cleanup_pids; do [ -n "$pid" ] || continue; kill -INT "$pid" 2>/dev/null || :; if wait "$pid"; then rc=0; else rc=$?; fi; printf '%s %s\n' "$pid" "$rc" >> "$LOG/owned-wait.rc"; done
cleanup_csv=$(printf '%s\n' $cleanup_pids | awk 'NF {printf "%s%s", separator, $1; separator=","}')
if [ -n "$cleanup_csv" ]; then ps -p "$cleanup_csv" -o pid=,ppid=,stat=,command= > "$LOG/processes-after-stop.txt" 2>&1 || :; else : > "$LOG/processes-after-stop.txt"; fi
find "$WORK" \( -type s -o -type p \) -print > "$LOG/ipc-remnants.txt"
test ! -s "$LOG/processes-after-stop.txt"; test ! -s "$LOG/ipc-remnants.txt"
```

残留、wait非0、process所有者が識別できない場合はcleanup未達とし、外部processや既存socketを削除しない。

### T09 TS・counter・終了値判定

- **適用対象/頻度:** 全capture/soak。
- **準備:** Python 3.11以上を`python3 --version`で確認し、手順書checkoutの[`ts-check.py`](release-validation/ts-check.py)を使う。候補ごとに新規作成した`$LOG`を使う。
- **実操作/利用者への依頼:** POSIXでは各TSに`python3 <runbook-checkout>/docs/release-validation/ts-check.py "$LOG/<receiver>.ts" > "$LOG/<receiver>.ts-check.json"`を実行する。Windowsは同じhelperを`python <runbook-checkout>\docs\release-validation\ts-check.py "$LOG\<receiver>.ts"`で実行し、標準出力を`$LOG`へ保存する。全capture/soakの各TSで実行し、候補daemonのdrop/USB counterも記録する。
- **期待値/終了値:** 全TSが製品SPEC基準を満たし通常取得exit 0。非空file/counter 0だけでpassにしない。
- **停止/cleanup:** 元TS/hash/logを残してT08 cleanup。
- **記録:** `python3 --version`、helperのGit SHA、TS hash、helperのJSON/終了値、sync/TEI/continuity/drop/counter、client exit。

Windowsで各TSを確認するcommand:

```powershell
$helper = '<runbook-checkout>\docs\release-validation\ts-check.py'; $ts = Join-Path $LOG '<receiver>.ts'
$json = Join-Path $LOG '<receiver>.ts-check.json'; $err = Join-Path $LOG '<receiver>.ts-check.err'
& python $helper $ts 1> $json 2> $err; $parserRc = $LASTEXITCODE
$parserRc | Set-Content (Join-Path $LOG '<receiver>.ts-check.rc')
if ($parserRc -ne 0) { throw "TS parser failed: $parserRc" }
```

### T10 native PC/SC・WinSCard

- **適用対象/頻度:** 対応OS/APIのみ。非対応製品は理由付きN/A。
- **準備:** native OS adapter/consumerとcandidate IFD/DLL pathを用意。
- **実操作/利用者への依頼:** Linux Appendix Aのisolated pcsc-liteでpcsc_scan -r/-cと既存bcas-safe-probe。macOSはE04 [native PC/SC](#e04-macos-native-pcsc)でcandidate bundle/reader template、Homebrew pcsc-lite、reader指定SCard consumerを実行する。Windowsは既存native transaction consumerがある場合のみ。
- **期待値/終了値:** CLI CARD/CI smokeをnative transaction合格にしない。実consumer/adapter/path/exitが必要。
- **停止/cleanup:** consumer/isolated pcscd通常停止。system PCSC serviceを変更しない。
- **記録:** OS/ABI、module/DLL absolute path、consumer、reader/card、operation/exit。

### T11 LNB・非対応要求の拒否

- **適用対象/頻度:** 必要時。給電対応profileだけ実測。
- **準備:** 対応profile、meter、独立給電の状態を確認。
- **実操作/利用者への依頼:** 対応機は`asicend --device usb:<BUS-PORT> --instance rc --runtime-dir "$RT" --allow-lnb-power`でdaemonを起動し、`asicenctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc lnb-on` / `lnb-off`を各実行してmeter測定する。将来、LNB制御非対応profileを持つ実機が利用可能になった場合だけ、sourceで非対応と確認した機器を選び、誤選択時に出力がないことをmeterで確認してから、同じdaemon commandと公開`asicenctl` commandを使う。`--allow-lnb-power`付きで`lnb-on`を一度呼び、未許可フラグによる一般拒否とprofile拒否を区別する。非対応profileを一意に確認できない候補では実行しない。
- **期待値/終了値:** 対応機はmeter実測を記録する。非対応profileの公開APIは`UNSUPPORTED`・exit 3を返し、GPIO/物理給電は切り替わらずmeter値も変化しないことを確認する。API拒否だけでは物理給電不変の確認にならない。
- **停止/cleanup:** daemonを通常停止し、非対応profileでは給電要求を追加せず0Vと通常配線復帰を確認する。
- **記録:** model/profile、command/response/exit、meter値と条件、cleanup。現所持ASICEN機器は全てLNB対応のため非対応profile拒否の実機確認は未実施・現在実行不可と記録する。対応機試験やmockを代替合格にしない。

### T12 複数device・daemon・access

- **適用対象/頻度:** 必要時、複数device/selector/access変更時。
- **準備:** USB port/selectorとinstance/runtimeをdeviceごとに割り当てる。
- **実操作/利用者への依頼:** asicend --list-jsonで複数USB pathを列挙しport/instance/runtimeを分ける。4 receiver同時はT03。driver unbindしない。
- **期待値/終了値:** device/selector対応一意。別deviceの結果を混同せず、要求時は各sideを独立判定。
- **停止/cleanup:** 各client/daemon個別wait/stopしFD/IPC/socket確認。
- **記録:** device/port/PID/runtime、各exit/継続/再取得/access/cleanup。

### T13 連続受信・頻度

- **適用対象/頻度:** 毎releaseおよび全model初回/major。
- **準備:** 確定頻度表の対象archive/model/timeをmatrixへ展開する。
- **実操作/利用者への依頼:** 毎release代表Linux x86_64 glibc 2h/全archive native short; 初回全modelは代表機all-free-channel 10s、macOS/Windows 30m、他ABI 10m。
- **期待値/終了値:** 指定連続時間完了かつTS条件合格。追加soakは明示ユーザー決定分のみ。
- **停止/cleanup:** T08通常stopと全resource cleanup。
- **記録:** model/OS/ABI/archive、時間、TS/counter、FD/RSS、未実施理由。

### T14 失敗・リリース判定

- **適用対象/頻度:** 全candidate gate完了後。
- **準備:** source/archive SHAと全T-IDの結果を版別記録へ用意。
- **実操作/利用者への依頼:** version recordで全T-IDをpass/fail/未実施/対象外分類。未完了を公開passにしない。
- **期待値/終了値:** failure/unsupported/未実施/環境不能/candidate欠落をpassにしない。
- **停止/cleanup:** candidateと失敗証拠を保持。tag/releaseは別依頼。
- **記録:** T-ID、candidate/host/model、state、log、blocker、判定根拠。

N/Aは製品機能上該当しない項目だけに使う。OS/ABI候補のunsupported、candidate欠落、未実施、失敗、環境不能は版別結果で未達とし、passに読み替えない。手順に記した期待値は実測結果ではない。

## 対象アーカイブ・環境から全試験への対応

選択したarchive/E-IDごとに、下表のT00–T14を同じrun記録へ実施する。各T見出しは準備・実操作・期待値/終了値・停止/cleanup・記録を含む手順本体であり、表は対象環境からその全項目へ直接辿る索引である。環境表に載る別OS/ABIの結果や過去runで代用しない。

| archive / E-ID / 実行環境 | T00–T14の手順リンク |
|---|---|
| `linux-glibc-x86_64` / [E03](#3-配布アーカイブ別手順) / Linux x86_64/glibc | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `linux-musl-x86_64` / [E02](#3-配布アーカイブ別手順) / Linux x86_64/musl | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `linux-glibc-aarch64` / [E15-G](#3-配布アーカイブ別手順) / Linux aarch64/glibc | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `linux-musl-aarch64` / [E15-M](#3-配布アーカイブ別手順) / Linux aarch64/musl | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `darwin-arm64` / [E04](#3-配布アーカイブ別手順) / macOS arm64 | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `android-aarch64` / [E05](#4-cardpcscusb) / Android Termux/aarch64 | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `android-armv7a` / [E06](#4-cardpcscusb) / Android Termux/armv7a | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `android-x86_64` / [E07](#4-cardpcscusb) / Android Termux/x86_64 | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |
| `win-candidate` / [E17](#windowsネイティブwinscard) / Windows 11 x86_64 candidate（正式配布archive外） | [T00](#t00-候補配布物固定) · [T01](#t01-実行環境アクセス権入力) · [T02](#t02-列挙selectorready確認) · [T03](#t03-最大同時受信) · [T04](#t04-不足波全無料channel) · [T05](#t05-受信中の状態確認制御) · [T06](#t06-card物理挿抜復帰) · [T07](#t07-usb待機中受信中の切断と再取得) · [T08](#t08-通常停止cleanup) · [T09](#t09-tscounter終了値判定) · [T10](#t10-native-pcscwinscard) · [T11](#t11-lnb非対応要求の拒否) · [T12](#t12-複数devicedaemonaccess) · [T13](#t13-連続受信頻度) · [T14](#t14-失敗リリース判定) |

OS/ABI固有の起動方法は以下の環境recipeと§3のarchive手順を使う。試験の期待・cleanup・記録項目はリンク先T本文に従う。機能対象外だけを理由付きN/Aとし、候補未入手・unsupported・未実施・環境不足は未達で記録する。

## 0. 共通配布アーカイブと共通手順

各冊の「共通実行環境表」は、Stable release gateで確認する正式配布artifactとnative/canonical runtimeの対応matrixである。環境IDはOS/ABIとUSB access boundaryを識別し、host名や試験状態を表さない。正式な対象archiveはREADME/SPECと各版のmatrixに従う。

| 共通要素 | 役割 |
|---|---|
| 環境ID | OS/ABIとruntime/access条件の識別子。host名・実施状態は版別記録に残す。 |
| 配布artifact matrix | 上の環境表で正式archiveと試験環境を対応付ける。追加の移植性確認環境はこのmatrixへ含めない。 |
| T00–T14 | 共通の試験項目・操作・期待値・cleanup・記録項目を示す。 |
| 共通コマンド | CLIやOS固有の実コマンドは製品ごとに異なる。各環境表の「配布artifact / 手順」リンク先と続くrecipeを使い、製品間で同じCLIを仮定しない。 |
| 状態語彙 | 実施状態は下の共通語彙を版別記録で使う。試験結果の合否とは別に記録する。 |

### 検証状態の共通語彙

| 状態 | 意味 |
|---|---|
| `継承` | 同一model・runtime/access path・featureで、artifact同一性または非影響の根拠がある既存証拠を適用する。 |
| `今回再検証` | 今回のcandidateで対象を実行し、結果を記録する。 |
| `未認定` | 対象の現行証拠がなく、合格を示せない。 |
| `対象外` | 製品仕様の対象外に限る。未実施・環境不能・candidate欠落・失敗には使わない。 |

## 1. 候補と配布物

候補source commitの `build_userland.yml` によるcandidate artifactを使い、run/ref/SHA、artifact ID、全archive hash、source manifest、license/provenanceを保存する。通常release配布binaryはLinux glibc/musl x86_64/aarch64、macOS arm64、Android aarch64/armv7a/x86_64の8 target。Windows ZIPは通常release aggregateに接続されない。Windowsが該当版の検証対象なら同候補runの `win-candidate` artifactを使う。source archiveも検査する。空の作業directoryへ展開し、同梱checksumを確認する。

```sh
mkdir -p candidate
gh run download <candidate-run-id> --name release-candidate --dir candidate
(cd candidate && sha256sum -c SHA256SUMS)
```

通常release aggregateに含まれないWindows ZIPは配布物として数えない。適用版にWindows native検証が含まれる場合はこの区別を保ち、通常配布物にないことを理由にnative確認を除外しない。欠落・hash不一致・source/licenseの不整合は候補失敗。

## 2. 機器非接続gate

候補commitの全CI build/test/package/audit job成功を確認する。README記載のnative・cross build、unit test、sanitizer、package展開試験、source archive再buildを行い、出力と終了値を保存する。Linux/macOSで `asicend --version`、`asicen-ts --version`、`asicenctl --version`、`asicend --list-json` を実行し、版情報と妥当なinventory JSONを確認する。USBなしinventoryはscan incomplete等の状態を区別して示す。`--list-json` はdevice ownership不要。異常引数・未知optionのexitとstderrをREADMEのCLI契約に照合する。

## 3. 配布アーカイブ別手順

毎release、代表Linux x86_64機で2時間連続受信し、全配布archiveをnative OS/ABIで短時間回帰する。初回/major追加確認ではmacOS/Windowsは各30分、残るOS/ABIは各10分連続受信し、全無料channel各10秒は代表機だけで行う。Windowsが対象の版ではnative確認に同source candidateの `win-candidate` artifactを使い、通常配布binaryの代替とは扱わない。

候補archiveをnative hostに展開し、`asicend --list-json` でUSB位置とmodelを確認する。値は現地の `BUS-PORT`、firmware、runtimeへ置換する。USB複数機能機種では確認済み内部hubで相方が一意な場合以外、両positionを指定する。

以下のT27単体commandは起動・TS出力のsmoke確認用であり、PX-W3U2/W3U3の4番組同時録画を満たす判定には使わない。4 client同時の開始・各TS/counter/終了値は共通シナリオAで確認する。

```sh
RT=$(mktemp -d "${TMPDIR:-/tmp}/asicen-release.XXXXXX")
asicend --device usb:<BUS-PORT[,BUS-PORT]> --instance rc --firmware <FIRMWARE> --runtime-dir "$RT" & P=$!
asicenctl --instance rc --runtime-dir "$RT" status
asicenctl --instance rc --runtime-dir "$RT" list
asicen-ts --instance rc --runtime-dir "$RT" --receiver 1 --channel T27 --time <SECONDS> --output t27.ts
```

statusがready、listにreceiverが存在すること、client終了値0、TSが188-byte packet列として同期すること、daemonのpacket/sync/TEI/continuity統計を確認する。変更に関係するchannel/receiverを受信し、チャンネル名・周波数・TSID等は現地channel planから列挙してログへ実値を記録する。衛星は実機のreceiver割当とREADMEに従い `--channel BS01_0 --lnb-voltage 0` を使う。出力非空だけをpassとせず、README/SPECのTS基準で判定し、失敗試行を消さない。

受信停止後、同じdaemon/runtimeで同じ受信を再取得し、normal exitと再取得成功を確認する。機種差があるので使うreceiverとT/Sの排他条件を記録する。

## 4. CARD・PC/SC・USB

`asicenctl` は `card-status` / `card-atr` / `card-reset` / `card-apdu` を公開する。各段階で以下を実行し、statusのreader数と候補profileを記録する。

```sh
asicenctl --runtime-dir "$RT" --instance rc card-status
asicenctl --runtime-dir "$RT" --instance rc card-atr
asicenctl --runtime-dir "$RT" --instance rc card-reset
for i in 1 2 3 4 5 6 7 8 9 10; do
  if asicenctl --runtime-dir "$RT" --instance rc card-apdu 90:30:00:00:00 > "$LOG/card-$i.apdu"; then rc=0; else rc=$?; fi
  echo "$rc" > "$LOG/card-$i.rc"
done
```

現行の公開`asicenctl`は pinned Coreの`card-apdu HEX`単発送信を使う。カードreaderを公開する実機でstatus present、ATR取得、reset後のATR、上の通常コマンド10回それぞれの終了値を確認する。カード抜去を依頼してからstatusが不在となり、ATR/APDUがCARD_REMOVED/NO_CARD (exit 9) となること、再挿入後にstatus/ATR/reset/APDUが戻ることを確認する。生ATR、カードID、APDU応答本文は公開記録に保存しない。未対応またはreaderなしprofileでのエラーはそのprofileの実測として記録し、製品全体の未実装と一般化しない。

hardware pathは `W3u3CardMailboxHardware` によるmodel-specific mailbox adapterである。Linuxでは専用test hostのisolated pcsc-liteを使い、productionのPC/SC serviceには接続しない。候補の`reader.conf.d/asicen-userland.conf`で次の4値を候補・実行daemonに合わせ、readerをASICEN 1つだけに絞る。

| 設定値 | 設定内容 |
|---|---|
| `@ASICEN_RUNTIME_DIR@` | `asicend`の`--runtime-dir`と同じ絶対path |
| `@ASICEN_INSTANCE@` | `asicend`の`--instance`と同じ値 |
| `@ASICEN_IFD_LIBRARY@` | 候補のLinux `ifd/asicen-userland-ifd.so`絶対path |
| `@ASICEN_ACCESS@` | 同一accountで動かす`user`。共有groupを使う場合はdaemon/consumer/pcscdのpermissionを合わせる |

専用pcsc-lite prefixとその`libpcsclite.so.1`を使い、reader config directoryだけを登録してforeground daemonを別processとして起動する。候補archiveを展開したdirectoryをcurrent directoryとし、同じ`RT`/`rc`で候補`asicend`を起動済みとする。準備からprobe buildまでのsource、template生成、固定pathはAppendix Aに示す。以下の例は`WORK`, `PREFIX`, `IPC`, `CONF`, `LOG`, `IFD`, `RT`をその例に従い設定済みとする。

```sh
mkdir -p "$LOG"
LD_LIBRARY_PATH="$PREFIX/lib" "$PREFIX/sbin/pcscd" --foreground \
  --config "$CONF" > "$LOG/pcscd.log" 2>&1 &
PCSC_PID=$!
printf '%s\n' "$PCSC_PID" > "$LOG/pcscd.pid"

if PCSCLITE_CSOCK_NAME="$IPC/pcscd.comm" LD_LIBRARY_PATH="$PREFIX/lib" "$PCSC_SCAN" -r > "$LOG/readers.txt" 2>&1; then scan_rc=0; else scan_rc=$?; fi
printf '%s\n' "$scan_rc" > "$LOG/readers.rc"
set -o pipefail
if PCSCLITE_CSOCK_NAME="$IPC/pcscd.comm" LD_LIBRARY_PATH="$PREFIX/lib" "$PCSC_SCAN" -c 2>&1 | \
  sed -E 's/(ATR[[:space:]]*[:=]).*/\1 <redacted>/'
then scan_rc=0; else scan_rc=$?; fi
printf '%s\n' "$scan_rc" > "$LOG/card-state.rc"
if PCSCLITE_CSOCK_NAME="$IPC/pcscd.comm" LD_LIBRARY_PATH="$PREFIX/lib" "$WORK/bcas-safe-probe" > "$LOG/bcas-safe-probe.txt" 2>&1; then probe_rc=0; else probe_rc=$?; fi
printf '%s\n' "$probe_rc" > "$LOG/bcas-safe-probe.rc"
kill -INT "$PCSC_PID"
if wait "$PCSC_PID"; then pcscd_rc=0; else pcscd_rc=$?; fi
printf '%s\n' "$pcscd_rc" > "$LOG/pcscd.rc"
```

`pcsc_scan -r`はexit 0かつASICEN readerが1件、ATRを伏せた`pcsc_scan -c`はpipeline exit 0かつcard insertedを期待する。Appendix Aの既存`bcas-safe-probe`はPC/SC connect後の標準B-CAS初期設定、`get_init_status()`、read-only `get_id()`を行い、exit 0かつ`bcas_init=ok` / `init_status=ok` / `id_query=ok`を期待する。3つの`.rc`が全て`0`で、記録されたreaderが1件、cardがinserted、probeの3ラベルが`ok`の場合だけpassとする。probeはreaderを1つだけ登録したconfigから一意に選ぶ。カード不在・ATR/resetの独立確認は同じdaemon/runtime/instanceの`asicenctl card-status`, `card-atr`, `card-reset`を使い、ATR/resetのraw出力は`/dev/null`へ送り終了値だけを記録する。CLI CARD操作はnative PC/SC probe結果の代用ではない。raw ATR、card ID、status内識別値、APDU response bytesを出力・保存しない。consumerを終了したら`kill -INT "$PCSC_PID"`後に`wait "$PCSC_PID"`し、pcscd終了値を保存してからisolated socket/process消失を確認する。続いて`asicend`を通常停止し、test config/runtimeを削除する。system serviceは変更しない。

#### 付録A. Linux専用PC/SCと既存safe probeの準備

次のレシピは専用`WORK`だけに書き込み、system pcscd/serviceには触れない。`RT`は候補daemonの`--runtime-dir`、`rc`はその`--instance`、`IFD`は候補archive内`ifd/asicen-userland-ifd.so`のabsolute pathとする。必要なbuild toolsは`git`, `cmake`, C/C++ compiler, `pkg-config`, `python3`/venv, `ninja`, `tar`, `sha256sum`, `curl`。reader/card状態確認には`pcsc-tools`由来の`pcsc_scan`も必要。

Appendix Aを実行する前に、`pcsc_scan`が`pcsc-tools`から利用可能であることを確認して`PCSC_SCAN`へ絶対pathを設定する。未用意ならnative PC/SC gateは未実施として記録し、host/system PCSC serviceへ接続する代替手段を使わない。候補前提の専用library/socketは、`LD_LIBRARY_PATH="$PREFIX/lib"`と`PCSCLITE_CSOCK_NAME="$IPC/pcscd.comm"`で明示し、`ldd "$PCSC_SCAN"`が`$PREFIX/lib/libpcsclite.so.1`を指すことを確認する。いずれかが成立しない場合は実行せず未実施とする。

```sh
WORK=$(mktemp -d "${TMPDIR:-/tmp}/asicen-pcsc.XXXXXX")
PREFIX="$WORK/prefix"
IPC="$WORK/ipc"
CONF="$WORK/readers/asicen"
LOG="$WORK/log"
mkdir -p "$IPC" "$CONF" "$LOG" "$WORK/drivers"
python3 -m venv "$WORK/venv"
"$WORK/venv/bin/pip" install --disable-pip-version-check meson==1.7.0
MESON="$WORK/venv/bin/meson"

curl -fL https://deb.debian.org/debian/pool/main/p/pcsc-lite/pcsc-lite_2.3.3.orig.tar.xz -o "$WORK/pcsc-lite_2.3.3.orig.tar.xz"
printf '%s  %s\n' cdff7d7153a0b37aa74e26dfec89ec7dc5c5286aa21b91b903e38739d227e8e7 "$WORK/pcsc-lite_2.3.3.orig.tar.xz" | sha256sum -c -
tar -xJf "$WORK/pcsc-lite_2.3.3.orig.tar.xz" -C "$WORK"
"$MESON" setup "$WORK/pcsc-build" "$WORK/pcsc-lite-2.3.3" \
  --prefix "$PREFIX" -Dusb=false -Dlibusb=false -Dlibudev=false \
  -Dserial=true -Dipcdir="$IPC" -Dserialconfdir="$CONF" \
  -Dusbdropdir="$WORK/drivers" -Dpolkit=false -Dlibsystemd=false
"$MESON" compile -C "$WORK/pcsc-build"
"$MESON" install -C "$WORK/pcsc-build"
PCSC_SCAN=$(command -v pcsc_scan)
LD_LIBRARY_PATH="$PREFIX/lib" ldd "$PCSC_SCAN" | grep -F "$PREFIX/lib/libpcsclite.so.1"

git clone https://github.com/kazuki0824/recisdb-rs.git "$WORK/recisdb-rs"
git -C "$WORK/recisdb-rs" checkout --detach badc171f15bb010b972740c17a92cfe0cbfe8584
SRC="$WORK/recisdb-rs"
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig" cmake \
  -S "$SRC/b25-sys/externals/libaribb25" -B "$WORK/aribb25-build" \
  -DWITH_PCSC_PACKAGE=libpcsclite -DUSE_AVX2=OFF
cmake --build "$WORK/aribb25-build" --target aribb25-static --parallel

IFD=$(realpath ./ifd/asicen-userland-ifd.so)
sed -e "s|@ASICEN_RUNTIME_DIR@|$RT|g" \
    -e 's|@ASICEN_INSTANCE@|rc|g' \
    -e "s|@ASICEN_IFD_LIBRARY@|$IFD|g" \
    -e 's|@ASICEN_ACCESS@|user|g' \
    packaging/pcsc/reader.conf.d/asicen-userland.conf.in > "$CONF/asicen-userland.conf"
```

次の既存診断sourceを`$WORK/bcas_safe_probe.c`へ保存し、固定した`libaribb25`と隔離pcsc-lite client libraryへリンクする。このprobeは操作成功ラベルだけを出力し、ATR、card ID、status field、APDU response bytesは表示しない。

```c
#include <stdio.h>
#include <string.h>
#include "b_cas_card.h"

int main(void)
{
    B_CAS_CARD *card = create_b_cas_card();
    B_CAS_INIT_STATUS status;
    B_CAS_ID id;
    int init_rc, status_rc, id_rc;

    memset(&status, 0, sizeof(status));
    memset(&id, 0, sizeof(id));
    if (!card) {
        puts("bcas_create=fail");
        return 1;
    }
    init_rc = card->init(card);
    if (init_rc < 0) {
        puts("bcas_init=fail");
        card->release(card);
        return 2;
    }
    puts("bcas_init=ok");
    status_rc = card->get_init_status(card, &status);
    printf("init_status=%s\n", status_rc < 0 ? "fail" : "ok");
    id_rc = card->get_id(card, &id);
    printf("id_query=%s\n", id_rc < 0 ? "fail" : "ok");
    if (id.data && id.count > 0)
        memset(id.data, 0, (size_t)id.count * sizeof(*id.data));
    memset(&status, 0, sizeof(status));
    card->release(card);
    return status_rc < 0 || id_rc < 0 ? 3 : 0;
}
```

```sh
cc -I "$SRC/b25-sys/externals/libaribb25/aribb25" \
  "$WORK/bcas_safe_probe.c" "$WORK/aribb25-build/libaribb25.a" \
  -L "$PREFIX/lib" -Wl,-rpath,"$PREFIX/lib" \
  -lpcsclite -lstdc++ -lm -o "$WORK/bcas-safe-probe"
ldd "$WORK/bcas-safe-probe" | grep 'libpcsclite.so.1'
```

最後の`ldd`出力が`$PREFIX/lib/libpcsclite.so.1`を指すことを確認する。reader templateは実際の候補IFD/daemon runtime/instanceへ展開済みである。実行時はこのdoc本文の手順で`$PREFIX/sbin/pcscd`をbackground起動しPIDを保存する。`pcsc_scan`と`bcas-safe-probe`には専用libraryと`$IPC/pcscd.comm`を指すclient environmentを設定し、system serviceへの接続を避ける。candidateに対応するASICEN IFD buildと`bcas-safe-probe`のcompile/linkだけではCARD gateを満たさず、reader/card実行結果を別途記録する。

AndroidにはIFDがない。

#### E04 macOS native PC/SC

T10のnative consumerは以下を今回candidateごとに実施する。過去E04 recordは環境例であり、今回candidate合格へ流用しない。T00で固定した`release-candidate` runから配布archiveとmanifestを取得する。

```sh
set -eu
WORK=$(mktemp -d "${TMPDIR:-/tmp}/asicen-pcsc.XXXXXX"); chmod 700 "$WORK"
RUN_ID='<candidate-release-run-id>'
gh run download "$RUN_ID" --repo Khronos31/asicen-userland --name release-candidate --dir "$WORK/download"
cd "$WORK/download"; shasum -a 256 -c SHA256SUMS
ARCHIVE='<darwin-arm64 archive filename from this candidate>'
mkdir "$WORK/candidate"; tar -xzf "$ARCHIVE" -C "$WORK/candidate"; D="$WORK/candidate"
brew list --versions pcsc-lite >/dev/null 2>&1 || brew install pcsc-lite
brew list --versions pcsc-tools >/dev/null 2>&1 || brew install pcsc-tools
brew list --versions pkgconf >/dev/null 2>&1 || brew install pkgconf
BREW_PREFIX=$(brew --prefix); PCSC_PREFIX=$(brew --prefix pcsc-lite); PCSC_TOOLS_PREFIX=$(brew --prefix pcsc-tools); IPC_ROOT="$BREW_PREFIX/var/run"
IPC_SOCKET="$IPC_ROOT/pcscd.comm"; IPC_PIDFILE="$IPC_ROOT/pcscd.pid"
export LIBPCSCLITE_DELEGATE="$PCSC_PREFIX/lib/libpcsclite_real.1.dylib"
test -f "$LIBPCSCLITE_DELEGATE"
export DYLD_LIBRARY_PATH="$PCSC_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export PKG_CONFIG_PATH="$PCSC_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
mkdir -m 700 "$WORK/runtime" "$WORK/reader.conf.d" "$WORK/log"
RT="$WORK/runtime"; LOG="$WORK/log"; TOKEN="pcsc-$(date +%s)"; PORT='<BUS-PORT-from-asicend-list-json>'; FW='<candidate firmware path for the selected model>'
if [ -e "$IPC_SOCKET" ] || [ -e "$IPC_PIDFILE" ]; then
  echo 'Homebrew PC/SC IPC is occupied; stop without touching that service.' >&2; exit 1
fi
if lsof -nU "$IPC_SOCKET" > "$LOG/ipc-preflight-owner.txt" 2>&1; then
  echo 'Homebrew PC/SC IPC has an owner; stop without service intervention.' >&2; exit 1
fi
printf '%s\n' 'socket=absent pidfile=absent lsof=unowned' > "$LOG/ipc-preflight.txt"

ASICEND_PID=; PCSC_PID=; PRIMARY_RC=0; CLEANUP_RC=0; CLEANUP_DONE=0; CURRENT_STEP=preflight
owned_child() {
  owned_info=$(ps -p "$1" -o ppid=,stat= 2>/dev/null)
  owned_ppid=$(printf '%s\n' "$owned_info" | awk '{print $1}')
  owned_stat=$(printf '%s\n' "$owned_info" | awk '{print $2}')
  [ "$owned_ppid" = "$$" ] && case "$owned_stat" in *Z*) return 1 ;; esac
}
stop_owned() {
  stop_role=$1; stop_pid=$2; stop_mode=already-exited; signal_rc=not-sent; term_rc=not-sent; kill_rc=not-sent
  if owned_child "$stop_pid"; then
    date -u +%FT%TZ > "$LOG/$stop_role.stop-requested-at"
    stop_mode=SIGINT-cooperative
    if kill -INT "$stop_pid" 2>/dev/null; then signal_rc=0; else signal_rc=$?; fi
    stop_attempt=0
    while [ "$stop_attempt" -lt 30 ] && owned_child "$stop_pid"; do sleep 1; stop_attempt=$((stop_attempt + 1)); done
    if owned_child "$stop_pid"; then
      stop_mode=SIGTERM-after-SIGINT-observation-timeout; CLEANUP_RC=1
      if kill -TERM "$stop_pid" 2>/dev/null; then term_rc=0; else term_rc=$?; fi
      stop_attempt=0
      while [ "$stop_attempt" -lt 10 ] && owned_child "$stop_pid"; do sleep 1; stop_attempt=$((stop_attempt + 1)); done
      if owned_child "$stop_pid"; then
        stop_mode=SIGKILL-after-SIGTERM-observation-timeout; CLEANUP_RC=1
        if kill -KILL "$stop_pid" 2>/dev/null; then kill_rc=0; else kill_rc=$?; fi
      fi
    fi
  fi
  if wait "$stop_pid"; then wait_rc=0; else wait_rc=$?; fi
  date -u +%FT%TZ > "$LOG/$stop_role.wait-completed-at"
  printf '%s\n' "$stop_mode" > "$LOG/$stop_role.stop-mode"
  printf '%s\n' "$signal_rc" > "$LOG/$stop_role.sigint.rc"
  printf '%s\n' "$term_rc" > "$LOG/$stop_role.sigterm.rc"
  printf '%s\n' "$kill_rc" > "$LOG/$stop_role.sigkill.rc"
  printf '%s\n' "$wait_rc" > "$LOG/$stop_role.wait.rc"
}
cleanup_owned() {
  cleanup_input_rc=$1; cleanup_input_step=$2
  if [ "$CLEANUP_DONE" -eq 1 ]; then
    if [ "$cleanup_input_rc" -ne 0 ]; then return "$cleanup_input_rc"; fi
    return "$CLEANUP_RC"
  fi
  CLEANUP_DONE=1; trap - 0; trap '' INT TERM HUP; set +e
  if [ "$PRIMARY_RC" -eq 0 ]; then PRIMARY_RC=$cleanup_input_rc; fi
  printf '%s\n' "$PRIMARY_RC" > "$LOG/primary.rc"
  if [ "$PRIMARY_RC" -ne 0 ]; then printf '%s\n' "$cleanup_input_step" > "$LOG/primary.step"; else printf '%s\n' 'none' > "$LOG/primary.step"; fi
  if [ -n "$PCSC_PID" ]; then stop_owned pcscd "$PCSC_PID"; fi
  if [ -n "$ASICEND_PID" ]; then stop_owned asicend "$ASICEND_PID"; fi
  if [ -e "$IPC_SOCKET" ]; then
    printf '%s\n' 'present' > "$LOG/ipc-socket.state"
    if lsof -nU "$IPC_SOCKET" > "$LOG/ipc-socket-owner.txt" 2>&1; then
      if awk 'NR > 1 {print $2}' "$LOG/ipc-socket-owner.txt" | grep -Fx "$PCSC_PID" >/dev/null; then
        printf '%s\n' 'owned-residual' > "$LOG/ipc-socket.owner-state"
      else
        printf '%s\n' 'other-or-unattributed-owner' > "$LOG/ipc-socket.owner-state"
      fi
    else
      printf '%s\n' 'no-owner-stale-or-unattributed' > "$LOG/ipc-socket.owner-state"
    fi
    CLEANUP_RC=1
  else
    printf '%s\n' 'absent' > "$LOG/ipc-socket.state"
  fi
  if [ -e "$IPC_PIDFILE" ]; then
    if IFS= read -r ipc_pidfile_value < "$IPC_PIDFILE"; then :; else ipc_pidfile_value=unreadable; fi
    printf '%s\n' "$ipc_pidfile_value" > "$LOG/ipc-pidfile.value"
    if [ "$ipc_pidfile_value" = "$PCSC_PID" ]; then
      printf '%s\n' 'owned-residual' > "$LOG/ipc-pidfile.state"
    else
      printf '%s\n' 'other-or-unattributed' > "$LOG/ipc-pidfile.state"
    fi
    CLEANUP_RC=1
  else
    printf '%s\n' 'absent' > "$LOG/ipc-pidfile.state"
  fi
  cleanup_pids=
  if [ -n "$PCSC_PID" ]; then cleanup_pids=$PCSC_PID; fi
  if [ -n "$ASICEND_PID" ]; then
    if [ -n "$cleanup_pids" ]; then cleanup_pids="$cleanup_pids,$ASICEND_PID"; else cleanup_pids=$ASICEND_PID; fi
  fi
  if [ -n "$cleanup_pids" ]; then
    if ps -p "$cleanup_pids" -o pid=,ppid=,stat=,command= > "$LOG/processes-after-stop.txt"; then PROCESS_RC=0; else PROCESS_RC=$?; fi
  else
    : > "$LOG/processes-after-stop.txt"; PROCESS_RC=0
  fi
  printf '%s\n' "$PROCESS_RC" > "$LOG/processes-after-stop.rc"
  if [ -s "$LOG/processes-after-stop.txt" ]; then CLEANUP_RC=1; fi
  find "$WORK" \( -type s -o -type p \) -print > "$LOG/work-ipc-remnants.txt"
  printf '%s\n' "$CLEANUP_RC" > "$LOG/cleanup.rc"
  trap - INT TERM HUP; set -eu
  if [ "$PRIMARY_RC" -ne 0 ]; then return "$PRIMARY_RC"; fi
  return "$CLEANUP_RC"
}
on_exit() {
  exit_input_rc=$?
  if [ "$PRIMARY_RC" -eq 0 ]; then PRIMARY_RC=$exit_input_rc; fi
  if cleanup_owned "$PRIMARY_RC" "$CURRENT_STEP"; then exit 0; else exit_code=$?; exit "$exit_code"; fi
}
trap 'exit 129' HUP; trap 'exit 130' INT; trap 'exit 143' TERM; trap 'on_exit' 0
CURRENT_STEP=asicend-start
"$D/asicend" --list-json > "$LOG/discovery.json"
"$D/asicend" --device "usb:$PORT" --instance "$TOKEN" --firmware "$FW" --runtime-dir "$RT" > "$LOG/asicend.log" 2>&1 & ASICEND_PID=$!
printf '%s\n' "$ASICEND_PID" > "$LOG/asicend.pid"
CURRENT_STEP=reader-config
sed -e "s|@ASICEN_RUNTIME_DIR@|$RT|g" -e "s|@ASICEN_INSTANCE@|$TOKEN|g" \
  -e "s|@ASICEN_IFD_LIBRARY@|$D/ifd/asicen-userland-ifd.bundle|g" -e 's|@ASICEN_ACCESS@|user|g' \
  "$D/reader.conf.d/asicen-userland.conf" > "$WORK/reader.conf.d/asicen-userland.conf"
CURRENT_STEP=pcscd-start
"$PCSC_PREFIX/sbin/pcscd" --foreground --debug --config "$WORK/reader.conf.d" > "$LOG/pcscd.log" 2>&1 & PCSC_PID=$!
printf '%s\n' "$PCSC_PID" > "$LOG/pcscd.pid"
```

Homebrew `pcsc-lite`の固定IPCが占有されている場合は既存process/socketに介入せず未実施で止める。Apple PCSC.framework / launchd serviceは停止・変更しない。Homebrew clientとdaemonを使い、別socket名指定だけでdaemon IPCを隔離できるとは扱わない。

次のshブロックも同じ対話shellで続けて実行する。途中でcommandが非0になった場合は登録済みtrapが一次終了値を保存してcleanupする。reader指定可能なnative consumerは標準APIでreader列挙、接続、status、APDU送信を行い、raw ATR、card ID、APDU response本文は出力しない。

```sh
CURRENT_STEP=pcsc-probe-source
cat > "$WORK/pcsc-probe.cpp" <<'CPP'
#include <PCSC/winscard.h>
#include <cstring>
#include <iostream>
#include <string>
int main(int argc, char **argv) {
  SCARDCONTEXT c = 0; SCARDHANDLE h = 0; int result = 1;
  if (argc != 2) return 64;
  do {
    if (SCardEstablishContext(SCARD_SCOPE_USER, nullptr, nullptr, &c) != SCARD_S_SUCCESS) break;
    DWORD n = SCARD_AUTOALLOCATE; LPSTR names = nullptr;
    if (SCardListReaders(c, nullptr, (LPSTR)&names, &n) != SCARD_S_SUCCESS) break;
    std::string reader; int matches = 0;
    for (char *p = names; *p; p += std::strlen(p) + 1) if (std::string(p).find(argv[1]) != std::string::npos) { reader = p; ++matches; }
    SCardFreeMemory(c, names);
    if (matches != 1) { result = 2; break; }
    DWORD active = 0;
    if (SCardConnect(c, reader.c_str(), SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &h, &active) != SCARD_S_SUCCESS) { result = 3; break; }
    DWORD state = 0, protocol = 0, rn = 256, an = 64; char r[256]; BYTE atr[64];
    if (SCardStatus(h, r, &rn, &state, &protocol, atr, &an) != SCARD_S_SUCCESS || state != SCARD_STATE_PRESENT || an == 0) { result = 4; break; }
    const BYTE cmd[] = {0x90,0x30,0x00,0x00,0x00}; BYTE response[258];
    const SCARD_IO_REQUEST *pci = active == SCARD_PROTOCOL_T0 ? SCARD_PCI_T0 : SCARD_PCI_T1;
    result = 0;
    for (int i = 0; i < 10; ++i) { DWORD len = sizeof(response); if (SCardTransmit(h, pci, cmd, sizeof(cmd), nullptr, response, &len) != SCARD_S_SUCCESS) { result = 5; break; } if (len < 2 || response[len-2] != 0x90 || response[len-1] != 0x00) { result = 6; break; } }
    if (result == 0) std::cout << "reader_match=1 card_present=1 apdu_sw9000=10\n";
  } while (false);
  if (h && SCardDisconnect(h, SCARD_LEAVE_CARD) != SCARD_S_SUCCESS && result == 0) result = 7;
  if (c && SCardReleaseContext(c) != SCARD_S_SUCCESS && result == 0) result = 7;
  std::cout << "probe_exit=" << result << "\n"; return result;
}
CPP
```

```sh
CURRENT_STEP=pcsc-probe-build
c++ -std=c++17 $(pkg-config --cflags libpcsclite) "$WORK/pcsc-probe.cpp" $(pkg-config --libs libpcsclite) -o "$WORK/pcsc-probe"
CURRENT_STEP=reader-discovery
for attempt in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30; do
  if "$PCSC_TOOLS_PREFIX/bin/pcsc_scan" -r > "$LOG/readers.txt" 2>&1 && grep -Fq 'ASICEN PLEX Internal Card Reader' "$LOG/readers.txt"; then break; fi
  sleep 1
done
grep -F 'ASICEN PLEX Internal Card Reader' "$LOG/readers.txt"
CURRENT_STEP=native-consumer
if "$WORK/pcsc-probe" 'ASICEN PLEX Internal Card Reader' > "$LOG/probe.txt" 2>&1; then PROBE_RC=0; else PROBE_RC=$?; fi
printf '%s\n' "$PROBE_RC" > "$LOG/probe.rc"
```

合格は今回candidate bundle pathがpcscd logにあり、readerが一意、probe exit 0、card present、APDU 10回全てSW `90 00`、出力`reader_match=1 card_present=1 apdu_sw9000=10`を満たすこと。exit 1=API/context、2=reader不一致、3=connect、4=status/card、5=transmit、6=SW不一致、7=release/disconnect、64=引数不正で、0以外は未達とする。reader名・終了値・成功件数のみ保存する。

```sh
CURRENT_STEP=normal-cleanup
cleanup_owned 0 "$CURRENT_STEP"
```

consumer終了後、記録済みの自分のPIDだけを停止し、各終了値を別記録する。process/socket/runtimeが残った場合は削除せずcleanup未達とする。正常cleanup確認後もcandidate、log、probe sourceは結果保存まで保持する。

USBはdaemon idleとTS受信中の両方で対象deviceを抜く。stream clientはUSB切断exit 7を確認し、戻した同一portの再列挙、旧daemonのcleanup、新daemonでのstatus/list、再受信を順に行う。自動hotplug復旧を期待しない。Termuxでは `termux-usb -l` と `termux-usb -r <usb-path>` で許可を取り、再列挙後にFDを再取得する。BUSYなら強制切断しない。

### WindowsネイティブWinSCard

Windows x86_64は回帰対象である。通常release aggregateにWindows ZIPがない場合は同じcandidate source runの`win-candidate` artifactを展開し、hash/manifest/provenanceを照合する。native Windows hostでUSB driverを自動変更せず、既に許可済みのlibusb access pathを記録する。実時間は共通頻度表に従い、初回/majorの30分連続受信は全無料channel取得とは別に記録する。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$FW = (Resolve-Path '.\firmware\<model-firmware>').Path
$RT = 'C:\asicen-rc-<run-id>'
if (Test-Path $RT) { throw "refusing to reuse existing runtime path: $RT" }
New-Item -ItemType Directory -Path $RT | Out-Null
$LOG = 'C:\asicen-e17-<run-id>-logs'
if (Test-Path $LOG) { throw "refusing to reuse existing log path: $LOG" }
New-Item -ItemType Directory -Path $LOG | Out-Null
& $D --list-json
$selector = 'usb:BUS-PORT[,BUS-PORT]'
& $D --device $selector --instance rc --firmware $FW --runtime-dir $RT
```

daemonはこのPowerShellで維持し、別のPowerShellを同じcandidate directoryで開いて同一instance/runtimeから照会・受信する。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$C = (Resolve-Path .\asicenctl.exe).Path
$B = (Resolve-Path .\asicen-ts.exe).Path
$RT = 'C:\asicen-rc-<run-id>'
$LOG = 'C:\asicen-e17-<run-id>-logs'
& $C --instance rc --runtime-dir $RT status
& $C --instance rc --runtime-dir $RT list
$capture = Start-Process -FilePath $B -ArgumentList @('--instance','rc','--runtime-dir',$RT,'--receiver','<ID>','--channel','T27','--time','30','--output',(Join-Path $PWD 't27.ts')) -PassThru -NoNewWindow -Wait
$capture.ExitCode
Get-FileHash -Algorithm SHA256 .\t27.ts
```

T07はdaemonを維持したままclientなしの状態でUSBを抜くidle runと、有限受信中のstream runを別々に行う。抜去を依頼する前にstatus/listとdaemon PIDを記録し、依頼から最大5分待つ。stream runでは`--time`を観測窓より長くしたclientを起動し、観測窓内の抜去終了を確認する。窓内に自然終了しない場合はtimeout未達として記録し、有限`--time`の終了まで待って実終了値を保存する。指定時間満了exit 0は切断合格ではない。抜去後にdaemonを通常停止し、再接続後は`--list-json`で同じUSB pathを確認して新しいruntimeで起動し、status/listと30秒受信を再実行する。T08のPID/終了値/process確認をこのWindows手順にも適用する。

```powershell
$B = (Resolve-Path .\asicen-ts.exe).Path
$RT = 'C:\asicen-rc-<run-id>'
$LOG = 'C:\asicen-e17-<run-id>-logs'
$R1 = '<receiver-1>'
$CAPTURE_SECONDS = '<finite-capture-duration-seconds-from-release-matrix>'
$OBSERVATION_LIMIT_SECONDS = '<finite-cut-observation-window-seconds>'
$client = Start-Process -FilePath $B -ArgumentList @('--instance','rc','--runtime-dir',$RT,'--receiver',$R1,'--channel','T27','--time',$CAPTURE_SECONDS,'--output',(Join-Path $LOG 'usb-cut.ts')) -PassThru -NoNewWindow -RedirectStandardOutput (Join-Path $LOG 'usb-cut.out') -RedirectStandardError (Join-Path $LOG 'usb-cut.err')
if (-not $client.WaitForExit([int]$OBSERVATION_LIMIT_SECONDS * 1000)) { 'timeout: capture still active at observation limit' | Out-File (Join-Path $LOG 'usb-cut-observation.txt'); $client.WaitForExit() }
$client.Refresh(); $client.ExitCode | Out-File (Join-Path $LOG 'usb-cut.rc')
```

WindowsではT03–T06も候補directoryの3 executableで行う。T03はW3U2/W3U3の公開最低要求である4 receiverを別TSへ同時起動し、他機種は公開receiver数を全て同時起動する。T04は現地channel planの各無料channelを個別に有限受信する。T05は受信中に別consoleからstatus/listと対応CARD controlを実行する。T06はdaemonを維持したまま利用者に物理挿抜を依頼し、status/ATR/reset/APDUを各段階で保存する。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$C = (Resolve-Path .\asicenctl.exe).Path
$B = (Resolve-Path .\asicen-ts.exe).Path
$RT = 'C:\asicen-rc-<run-id>'
$LOG = 'C:\asicen-e17-<run-id>-logs'
$R1 = '<receiver-1>'; $R2 = '<receiver-2>'; $R3 = '<receiver-3>'; $R4 = '<receiver-4>'
$CHANNEL_PLAN = @('<free-channel-1>','<free-channel-2>')
$CAPTURE_SECONDS = '30'
$jobs = @(
  [pscustomobject]@{ Receiver=$R1; Channel='T27'; Output='r1.ts'; Lnb=$false },
  [pscustomobject]@{ Receiver=$R2; Channel='T30'; Output='r2.ts'; Lnb=$false },
  [pscustomobject]@{ Receiver=$R3; Channel='BS01_0'; Output='r3.ts'; Lnb=$true },
  [pscustomobject]@{ Receiver=$R4; Channel='BS15_0'; Output='r4.ts'; Lnb=$true }
)
$clients = foreach ($job in $jobs) {
  $captureArgs = @('--instance','rc','--runtime-dir',$RT,'--receiver',$job.Receiver,'--channel',$job.Channel,'--time',$CAPTURE_SECONDS,'--output',(Join-Path $LOG $job.Output))
  if ($job.Lnb) { $captureArgs += @('--lnb-voltage','0') }
  Start-Process -FilePath $B -ArgumentList $captureArgs -PassThru -NoNewWindow
}
while ($clients.Where({ -not $_.HasExited }).Count -gt 0) {
  & $C --instance rc --runtime-dir $RT status 2>&1 | Tee-Object -Append (Join-Path $LOG 't05-status.txt')
  $LASTEXITCODE | Out-File -Append (Join-Path $LOG 't05-status.rc')
  & $C --instance rc --runtime-dir $RT list 2>&1 | Tee-Object -Append (Join-Path $LOG 't05-list.txt')
  $LASTEXITCODE | Out-File -Append (Join-Path $LOG 't05-list.rc')
  foreach ($client in $clients) { $client.Refresh() }
  Start-Sleep -Seconds 2
}
foreach ($client in $clients) { $client.Refresh(); [pscustomobject]@{ PID=$client.Id; ExitCode=$client.ExitCode } | Tee-Object -Append (Join-Path $LOG 't05-client-exit.txt') }
Get-FileHash -Algorithm SHA256 (Join-Path $LOG 'r1.ts'), (Join-Path $LOG 'r2.ts'), (Join-Path $LOG 'r3.ts'), (Join-Path $LOG 'r4.ts')

foreach ($channel in $CHANNEL_PLAN) {
  $file = Join-Path $LOG ($channel + '.ts')
  $client = Start-Process -FilePath $B -ArgumentList @('--instance','rc','--runtime-dir',$RT,'--receiver',$R1,'--channel',$channel,'--time','10','--output',$file) -PassThru -NoNewWindow -Wait
  [pscustomobject]@{ Channel=$channel; ExitCode=$client.ExitCode; File=$file }
}
function Wait-CardAction([string]$message) {
  if ([Console]::IsInputRedirected) { throw 'CARD action requires an interactive PowerShell console.' }
  Write-Host "$message Enterキーで完了を記録 (最大300秒): " -NoNewline
  $deadline = [DateTime]::UtcNow.AddSeconds(300)
  while ([DateTime]::UtcNow -lt $deadline) { if ([Console]::KeyAvailable -and [Console]::ReadKey($true).Key -eq [ConsoleKey]::Enter) { Write-Host; return }; Start-Sleep -Milliseconds 250 }
  throw 'CARD action was not confirmed within 300 seconds; record the gate as incomplete.'
}
& $C --instance rc --runtime-dir $RT card-status 2>&1 | Tee-Object (Join-Path $LOG 'card-before-status.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'card-before-status.rc')
foreach ($op in 'card-atr','card-reset') { & $C --instance rc --runtime-dir $RT $op *> $null; $LASTEXITCODE | Out-File (Join-Path $LOG "card-before-$op.rc") }
1..10 | ForEach-Object { & $C --instance rc --runtime-dir $RT card-apdu 90:30:00:00:00 *> $null; $LASTEXITCODE | Out-File (Join-Path $LOG "card-before-apdu-$_.rc") }
Wait-CardAction '同じCARDを物理的に抜いてください。'
& $C --instance rc --runtime-dir $RT card-status 2>&1 | Tee-Object (Join-Path $LOG 'card-removed-status.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'card-removed-status.rc')
& $C --instance rc --runtime-dir $RT card-atr *> $null; $LASTEXITCODE | Out-File (Join-Path $LOG 'card-removed-card-atr.rc')
& $C --instance rc --runtime-dir $RT card-apdu 90:30:00:00:00 *> $null; $LASTEXITCODE | Out-File (Join-Path $LOG 'card-removed-card-apdu.rc')
Wait-CardAction '同じCARDを再挿入してください。'
& $C --instance rc --runtime-dir $RT card-status 2>&1 | Tee-Object (Join-Path $LOG 'card-reinserted-status.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'card-reinserted-status.rc')
foreach ($op in 'card-atr','card-reset') { & $C --instance rc --runtime-dir $RT $op *> $null; $LASTEXITCODE | Out-File (Join-Path $LOG "card-reinserted-$op.rc") }
1..10 | ForEach-Object { & $C --instance rc --runtime-dir $RT card-apdu 90:30:00:00:00 *> $null; $LASTEXITCODE | Out-File (Join-Path $LOG "card-reinserted-apdu-$_.rc") }
```

T08ではclientごとの`ExitCode`保存後にdaemon consoleでCtrl+Cを入力し、daemon exit 0、process/FD/runtime cleanupを確認する。daemon終了後に同一instance/runtimeの`status`を実行しendpointが消えたことを確認する。T09は各TSのSHA-256とTSDuck `tsanalyze`のsync/TEI/continuity結果を記録する。T11は対応機種だけで、電圧計を使ったLNB on/offの公開command/実電圧を記録する。T12はdeviceごとに異なる`--instance`/`--runtime-dir`でdaemonを2つ起動し、client/終了値を分ける。T13は同じcaptureを確定頻度表の秒数で連続実行し、開始・定期・終了時のTS/counter/process/FD/RSSを保存する。T14は全T-IDの今回状態と証拠pathを版別記録へ集約し、未実施・候補欠落・unsupportedをpassにしない。

T12では別consoleごとに次のdaemon commandを使い、各consoleでCtrl+C後の終了値を記録する。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$FW = (Resolve-Path '.\firmware\<model-firmware>').Path
$RTA = 'C:\asicen-e17-<run-id>-a'; $RTB = 'C:\asicen-e17-<run-id>-b'
New-Item -ItemType Directory -Path $RTA, $RTB | Out-Null
$LOG = 'C:\asicen-e17-<run-id>-logs'
& $D --device 'usb:<PORT_A>' --instance a --firmware $FW --runtime-dir $RTA
```

別console Bでdaemonを起動する。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$FW = (Resolve-Path '.\firmware\<model-firmware>').Path
$RTB = 'C:\asicen-e17-<run-id>-b'
& $D --device 'usb:<PORT_B>' --instance b --firmware $FW --runtime-dir $RTB
```

両daemonがreadyになったら、3つ目のclient consoleから同じcandidateのclientを同時起動し、A抜去中もBのcaptureを続ける。各clientの実終了値を保存してから両daemonを通常停止し、client/endpoint cleanupを記録する。

```powershell
$B = (Resolve-Path .\asicen-ts.exe).Path
$LOG = 'C:\asicen-e17-<run-id>-logs'
$SA = '<finite-seconds-from-release-matrix>'; $SB = '<finite-seconds-from-release-matrix>'
$clientA = Start-Process -FilePath $B -ArgumentList @('--instance','a','--runtime-dir','C:\asicen-e17-<run-id>-a','--receiver','<receiver-a>','--channel','T27','--time',$SA,'--output',(Join-Path $LOG 'a.ts')) -PassThru -NoNewWindow
$clientB = Start-Process -FilePath $B -ArgumentList @('--instance','b','--runtime-dir','C:\asicen-e17-<run-id>-b','--receiver','<receiver-b>','--channel','T27','--time',$SB,'--output',(Join-Path $LOG 'b.ts')) -PassThru -NoNewWindow
```

別consoleで両instanceのstatus/listを保存してstreamingを確認し、A側deviceを抜去する。A/Bの実終了値を個別に保存する。

```powershell
$C = (Resolve-Path .\asicenctl.exe).Path
& $C --instance a --runtime-dir 'C:\asicen-e17-<run-id>-a' status 2>&1 | Tee-Object (Join-Path $LOG 'status-a.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'status-a.rc')
& $C --instance a --runtime-dir 'C:\asicen-e17-<run-id>-a' list 2>&1 | Tee-Object (Join-Path $LOG 'list-a.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'list-a.rc')
& $C --instance b --runtime-dir 'C:\asicen-e17-<run-id>-b' status 2>&1 | Tee-Object (Join-Path $LOG 'status-b.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'status-b.rc')
& $C --instance b --runtime-dir 'C:\asicen-e17-<run-id>-b' list 2>&1 | Tee-Object (Join-Path $LOG 'list-b.txt'); $LASTEXITCODE | Out-File (Join-Path $LOG 'list-b.rc')
$clientA.WaitForExit(); $clientA.Refresh(); $clientA.ExitCode | Out-File (Join-Path $LOG 'a.rc')
$clientB.WaitForExit(); $clientB.Refresh(); $clientB.ExitCode | Out-File (Join-Path $LOG 'b.rc')
```

```powershell
Get-Process asicend,asicen-ts -ErrorAction SilentlyContinue | Select-Object Id,ProcessName,WorkingSet64,HandleCount
Get-ChildItem -LiteralPath $RTA, $RTB -Force
& $C --instance a --runtime-dir $RTA status 2>&1 | Tee-Object "$LOG\status-a-after-stop.txt"
$LASTEXITCODE
& $C --instance b --runtime-dir $RTB status 2>&1 | Tee-Object "$LOG\status-b-after-stop.txt"
$LASTEXITCODE
```

Android/Termuxでは`termux-usb -l`の出力からUSB nodeを選び、`termux-usb -r`でpermissionを取得する。T02のdevice/path確認にはこのTermux列挙とpermission結果を使い、borrowed FD起動と併用できない`asicend --list-json`を実行しない。2機能機種はprimary/siblingのnodeを区別する。runtimeは短い`$PREFIX/tmp`直下へ作り、`--runtime-dir`/`--instance`を全clientで一致させる。E05/E06/E07それぞれで当該ABI archiveを使う。runtime VID:PIDなら下の通常起動へ進み、cold loaderならfirmware転送、再列挙、runtime FD grantを分けて実施する。transfer stageのexit 3とdiagnosticはruntime daemonの失敗ではなく、固定sourceが示す次のUSB grant段階への遷移なので、stageごとに保存する。

```sh
runtime_dir="$PREFIX/tmp/asicen-<run-id>"
mkdir -p "$runtime_dir" && chmod 700 "$runtime_dir"
termux-usb -l
termux-usb -r /dev/bus/usb/BBB/AAA
asicen-termux --usb-device /dev/bus/usb/BBB/AAA --runtime-dir "$runtime_dir" --instance rc
```

cold loaderのfirmware転送はTermux:APIから渡されたFDを`asicen-termux`経由で既存daemon入口へ渡す。single-function機器では次を実行し、固定sourceのtransfer完了diagnosticとexit 3の両方を保存する。続いて`termux-usb -l`で同じUSB pathに戻ったruntime VID:PID nodeを確認し、`termux-usb -r`でそのnodeのpermissionを取得して、同じruntime/instanceで`--firmware`を外した上のruntime起動を行う。exit 3だけ、別pathのruntime、または再列挙なしを成功に読み替えない。`termux-usb -r`で古いloader nodeを再利用しない。

```sh
firmware_path='./firmware/<model-firmware>'
termux-usb -l | tee "$runtime_dir/devices-loader.txt"
termux-usb -r /dev/bus/usb/BBB/AAA
if asicen-termux --usb-device /dev/bus/usb/BBB/AAA --firmware "$firmware_path" --runtime-dir "$runtime_dir" --instance rc > "$runtime_dir/firmware-stage.out" 2> "$runtime_dir/firmware-stage.err"; then stage_rc=0; else stage_rc=$?; fi
printf '%s\n' "$stage_rc" > "$runtime_dir/firmware-stage.rc"
test "$stage_rc" -eq 3 && grep -F 'firmware: --fd cannot follow the loader re-enumeration; upload separately, re-grant the runtime fd, then run again' "$runtime_dir/firmware-stage.err"
termux-usb -l | tee "$runtime_dir/devices-runtime.txt"
```

2-function機器では、固定sourceがpaired loaderの2 FD同時firmware指定を拒否するため、primary→sibling→runtime pairを一機能ずつ順に処理する。まずprimary loader nodeだけへpermissionを与え、上と同じ`asicen-termux --usb-device <primary-loader> --firmware "$firmware_path" --runtime-dir "$runtime_dir" --instance rc`を実行する。期待するのはfirmware transfer完了diagnosticとexit 3であり、次に`termux-usb -l`で同じprimary pathのruntime VID:PIDを確認する。続けてprimary runtime nodeを`--firmware`なしで一度grantすると、固定sourceはsibling restoreを要求してexit 3を返す。このとき`sibling restore: requested; grant the sibling loader FD and transfer its firmware`があり、かつprimary pathの隣接siblingがloader VID:PIDで列挙されることを確認する。そのsibling loader nodeを`--firmware "$firmware_path"`付きで一度grantし、firmware transfer完了diagnosticとexit 3を確認する。最後に`termux-usb -l`でprimary/sibling両方が同じ筐体のruntime VID:PIDへ戻ったことを確認し、両runtime nodeをprimary順に`asicen-termux --usb-device <primary-runtime> --usb-device <sibling-runtime> --runtime-dir "$runtime_dir" --instance rc`へ渡してdaemonを起動する。各exit 3は記載したstage diagnosticと次段のVID/PID/path確認がそろった場合だけstage遷移として扱う。単体のexit 3、異なるpathのruntime、期待node欠落、diagnostic不一致は失敗/未達とし、続行しない。daemonがreadyになった後status/list、capture exit 0、TSを確認して初めて合格とする。

paired機の段階呼出しは次のとおり。各runで`$PRIMARY_LOADER`、`$PRIMARY_RUNTIME`、`$SIBLING_LOADER`、`$PRIMARY_RUNTIME`/`$SIBLING_RUNTIME`を、その直前の`termux-usb -l`出力から選び、path/PID/VID:PIDと各終了値を保存する。各一時daemon invocationはfailure-on-exitのshell環境でも続くように終了値を`if`で採取する。

```sh
firmware_path='./firmware/<model-firmware>'
primary_loader='<primary-loader-usb-path>'
runtime_dir="$PREFIX/tmp/asicen-<run-id>"
mkdir -p "$runtime_dir" && chmod 700 "$runtime_dir"
termux-usb -l | tee "$runtime_dir/devices-primary-loader.txt"
termux-usb -r "$primary_loader"
if asicen-termux --usb-device "$primary_loader" --firmware "$firmware_path" --runtime-dir "$runtime_dir" --instance rc > "$runtime_dir/primary-firmware.out" 2> "$runtime_dir/primary-firmware.err"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" > "$runtime_dir/primary-firmware.rc"
test "$rc" -eq 3 && grep -F 'firmware: --fd cannot follow the loader re-enumeration; upload separately, re-grant the runtime fd, then run again' "$runtime_dir/primary-firmware.err"
# termux-usb -lでprimary runtime VID:PID/pathを確認後、primary runtime FDでsibling restoreを要求する。
termux-usb -l | tee "$runtime_dir/devices-primary-runtime.txt"
primary_runtime='<primary-runtime-usb-path-from-previous-output>'
termux-usb -r "$primary_runtime"
if asicen-termux --usb-device "$primary_runtime" --runtime-dir "$runtime_dir" --instance rc > "$runtime_dir/sibling-restore.out" 2> "$runtime_dir/sibling-restore.err"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" > "$runtime_dir/sibling-restore.rc"
test "$rc" -eq 3 && grep -F 'sibling restore: requested; grant the sibling loader FD and transfer its firmware' "$runtime_dir/sibling-restore.err"
# sibling restore diagnosticとsibling loader VID:PID/pathを確認してから続行する。
termux-usb -l | tee "$runtime_dir/devices-sibling-loader.txt"
sibling_loader='<sibling-loader-usb-path-from-previous-output>'
termux-usb -r "$sibling_loader"
if asicen-termux --usb-device "$sibling_loader" --firmware "$firmware_path" --runtime-dir "$runtime_dir" --instance rc > "$runtime_dir/sibling-firmware.out" 2> "$runtime_dir/sibling-firmware.err"; then rc=0; else rc=$?; fi
printf '%s\n' "$rc" > "$runtime_dir/sibling-firmware.rc"
test "$rc" -eq 3 && grep -F 'firmware: --fd cannot follow the loader re-enumeration; upload separately, re-grant the runtime fd, then run again' "$runtime_dir/sibling-firmware.err"
# termux-usb -lでprimary/sibling両方のruntime VID:PID/pathを確認してからdaemonを起動する。
termux-usb -l | tee "$runtime_dir/devices-runtime-pair.txt"
primary_runtime='<primary-runtime-usb-path-from-previous-output>'
sibling_runtime='<sibling-runtime-usb-path-from-previous-output>'
termux-usb -r "$primary_runtime"
termux-usb -r "$sibling_runtime"
asicen-termux --usb-device "$primary_runtime" --usb-device "$sibling_runtime" --runtime-dir "$runtime_dir" --instance rc
```

通常daemon起動後の各T本文にある`asicenctl`/`asicen-ts` client commandは別Termux sessionから候補archive内の同じclientを実行する。clientへUSB path/FDを渡さず、daemonの`--runtime-dir`/`--instance`を一致させる。終わりはlauncher sessionでCtrl+Cを送り、その終了値、daemon終了、process/FD/runtime cleanupを記録する。

`win-candidate`の`WinSCard.dll`はconsumerと同じdirectoryに置く。候補はCI専用で正式配布archiveの代替ではない。最初のPowerShell consoleで専用runtimeを作り、daemonを前景起動する。probeは2つ目のconsoleから実行し、Coreの既存APIを呼ぶ。Coreが選ぶASICEN reader名は`DTV userland (asicen:rc/reader-0)`である。DLL load/export smokeやoffline fixtureは実機transactionの証明ではない。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$FW = (Resolve-Path '.\firmware\<model-firmware>').Path
$RT = 'C:\asicen-e17-<run-id>'
if (Test-Path $RT) { throw "refusing to reuse existing runtime path: $RT" }
New-Item -ItemType Directory -Path $RT | Out-Null
$env:DTV_USERLAND_RUNTIME_DIR = $RT
$selector = 'usb:BUS-PORT[,BUS-PORT]'
& $D --device $selector --instance rc --firmware $FW --runtime-dir $RT
```

このconsoleはprobe cleanupまで開いたままにする。

```powershell
$ErrorActionPreference = 'Stop'
$DLL = (Resolve-Path .\WinSCard.dll).Path
$RT = 'C:\asicen-e17-<run-id>'
$env:DTV_USERLAND_RUNTIME_DIR = $RT
$env:PATH = (Split-Path $DLL) + ';' + $env:PATH
Set-Location (Split-Path $DLL)
$ctx = [IntPtr]::Zero; $card = [IntPtr]::Zero
try {
  $reader = 'DTV userland (asicen:rc/reader-0)'
  Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public struct ScardIoRequest { public uint Protocol; public uint Length; }
public static class WinSCardProbe {
  [DllImport("WinSCard.dll", EntryPoint="SCardEstablishContext", ExactSpelling=true)] public static extern int SCardEstablishContext(uint scope, IntPtr r1, IntPtr r2, out IntPtr context);
  [DllImport("WinSCard.dll", EntryPoint="SCardListReadersW", ExactSpelling=true, CharSet=CharSet.Unicode)] public static extern int SCardListReadersW(IntPtr context, IntPtr groups, System.Text.StringBuilder readers, ref uint length);
  [DllImport("WinSCard.dll", EntryPoint="SCardConnectW", ExactSpelling=true, CharSet=CharSet.Unicode)] public static extern int SCardConnectW(IntPtr context, string reader, uint share, uint protocols, out IntPtr card, out uint activeProtocol);
  [DllImport("WinSCard.dll", EntryPoint="SCardStatusW", ExactSpelling=true, CharSet=CharSet.Unicode)] public static extern int SCardStatusW(IntPtr card, System.Text.StringBuilder readers, ref uint readerLength, out uint state, out uint protocol, byte[] atr, ref uint atrLength);
  [DllImport("WinSCard.dll", EntryPoint="SCardTransmit", ExactSpelling=true)] public static extern int SCardTransmit(IntPtr card, ref ScardIoRequest sendPci, byte[] send, uint sendLength, IntPtr recvPci, byte[] recv, ref uint recvLength);
  [DllImport("WinSCard.dll", EntryPoint="SCardDisconnect", ExactSpelling=true)] public static extern int SCardDisconnect(IntPtr card, uint disposition);
  [DllImport("WinSCard.dll", EntryPoint="SCardReleaseContext", ExactSpelling=true)] public static extern int SCardReleaseContext(IntPtr context);
}
'@
  $active = [uint32]0
  $rc = [WinSCardProbe]::SCardEstablishContext(2, [IntPtr]::Zero, [IntPtr]::Zero, [ref]$ctx)
  if ($rc -ne 0) { throw ('SCardEstablishContext rc=' + $rc) }
  [uint32]$length = 0
  $rc = [WinSCardProbe]::SCardListReadersW($ctx, [IntPtr]::Zero, $null, [ref]$length)
  if ($rc -ne 0 -and $rc -ne -2146435064) { throw ('SCardListReadersW size rc=' + $rc) }
  $names = New-Object System.Text.StringBuilder ([int]$length)
  $rc = [WinSCardProbe]::SCardListReadersW($ctx, [IntPtr]::Zero, $names, [ref]$length)
  if ($rc -ne 0 -or $names.ToString() -notlike ('*' + $reader + '*')) { throw ('reader missing; rc=' + $rc) }
  $rc = [WinSCardProbe]::SCardConnectW($ctx, $reader, 2, 3, [ref]$card, [ref]$active)
  if ($rc -ne 0) { throw ('SCardConnectW rc=' + $rc) }
  [uint32]$readerLength = 256; [uint32]$state = 0; [uint32]$protocol = 0; [uint32]$atrLength = 64
  $readerNames = New-Object System.Text.StringBuilder 256
  $atr = New-Object byte[] 64
  $rc = [WinSCardProbe]::SCardStatusW($card, $readerNames, [ref]$readerLength, [ref]$state, [ref]$protocol, $atr, [ref]$atrLength)
  if ($rc -ne 0) { throw ('SCardStatusW rc=' + $rc) }
  Write-Host "reader=$reader state=$state protocol=$protocol ATR-length=$atrLength"
  $sendPci = New-Object ScardIoRequest
  $sendPci.Protocol = $active
  $sendPci.Length = [uint32][Runtime.InteropServices.Marshal]::SizeOf([ScardIoRequest])
  [byte[]]$send = 0x90,0x30,0x00,0x00,0x00
  [byte[]]$recv = New-Object byte[] 258
  [uint32]$recvLength = $recv.Length
  $rc = [WinSCardProbe]::SCardTransmit($card, [ref]$sendPci, $send, [uint32]$send.Length, [IntPtr]::Zero, $recv, [ref]$recvLength)
  if ($rc -ne 0) { throw ('SCardTransmit rc=' + $rc) }
  Write-Host "APDU=90:30:00:00:00 transmit=success response-length=$recvLength response-body=redacted"
  $rc = [WinSCardProbe]::SCardDisconnect($card, 0); $card = [IntPtr]::Zero
  if ($rc -ne 0) { throw ('SCardDisconnect rc=' + $rc) }
  $rc = [WinSCardProbe]::SCardReleaseContext($ctx); $ctx = [IntPtr]::Zero
  if ($rc -ne 0) { throw ('SCardReleaseContext rc=' + $rc) }
  Write-Host 'status=PASS'
}
finally {
  if ($card -ne [IntPtr]::Zero) { [void][WinSCardProbe]::SCardDisconnect($card, 0) }
  if ($ctx -ne [IntPtr]::Zero) { [void][WinSCardProbe]::SCardReleaseContext($ctx) }
  Remove-Item Env:DTV_USERLAND_RUNTIME_DIR -ErrorAction SilentlyContinue
}
```

2つのconsoleで同じ固定`$RT`を使う。`BUS-PORT`、firmware、model、DLL SHA、reader list/status、各API戻り値を保存する。カードを必要とする場合は利用者が挿入し、ATR本文・カードID・APDU応答本文を保存しない。native API probeの実行結果がない段階では未実施であり、fixtureはAPI形状の参照に限る。probeの期待値はcontext/list/connect/status/transmit/disconnect/releaseがすべてsuccess、指定readerが列挙されること。probe後はdaemon consoleでCtrl+Cを入力し、終了値を記録してprocess/FD/runtime残留がないことを確認してから専用runtimeを削除する。

## 5. LNB・連続受信・cleanup

PX-W3U2/W3U3/V2などREADMEで15V対応が明示された機種のみ、壁設備と他給電源を分離し、衛星端子へテスターを接続する。停止状態から `--allow-lnb-power` を付けてdaemonを起動し、`asicenctl --runtime-dir "$RT" --instance rc lnb-on` 後に実測15V、`asicenctl --runtime-dir "$RT" --instance rc lnb-off` 後に要求0と実測0Vを確認する。独立要求と受信部 `--lnb-voltage` は別の状態として記録する。非対応機はsoftware要求も試さない。daemon停止後0Vと通常配線への復帰を確認する。

Soakでは代表Linux x86_64機の2時間連続受信を実施する。開始・定期・終了時にTS/counters、process/FD/RSS、channel、終了値を記録する。終了後はclient/daemonを正常停止し、status/process/runtime socketを確認して後始末する。候補SHA/archive hash、host/OS/arch/model、firmware、channel、操作、実コマンド、期待/実測、エラー、未確認事項をrelease recordへ保存する。全gateを終えた後にだけ結果文書をcommitし、code/build inputsが候補と同一であることを確認する。正式tag/releaseは別途の承認対象。

## 6. 製品固有の適用範囲

Windowsの配布経路は `win-candidate` が通常release aggregateへ接続されていない点を踏まえる。LNBは対応3機種のみ、receiver/T-S割当と再選局可能性は機種別README表に従う。カードや給電機能がないprofileに試験を要求せず、各claimを `今回再検証` / `継承` / `未認定` / `対象外` で記録する。
