# Stable リリース前検証手順

この文書はASICEN Stable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように版別文書へ記録します。公開契約は [`SPEC.md`](../SPEC.md)、CLI構文・機種能力は [`README.md`](../README.md) と各コマンドの `--help` が正です。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは30分以上の連続TS取得を指す。ASICENの標準機種はPX-W3U3。

| 区分 | 適用頻度 | ASICENでの確認 |
|---|---|---|
| 機種初回 | PX-W3U3を代表機で2時間連続受信し、毎releaseの回帰項目を代表機だけで行う。必要時項目も通し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ記録する。 | READMEのW3U3能力に従い、地上波/衛星、CARD、対応LNBを確認。 |
| 回帰（毎release） | 代表機2時間連続受信。全配布archiveの各OS/ABIで短時間実機回帰、`--list-json`、機種の最大能力による同時受信とTS/counter/終了値を確認する。最大試験で地上/衛星3波が不足する場合は不足分を短時間取得する。受信中status、反復APDU、正常停止とprocess/FD/IPC残留なし、CARD挿抜、USB抜去/再挿入を確認する。Windows/WinSCard・Linux/macOS PC/SC経路も各対象OSで行う。 | W3U3の公開receiver slotは0/1の2つ。README記載の現行実装上限は同時stream 1本だが、これは機種本来の最大能力を示す根拠ではない。最大能力を確認したうえで試験し、候補がその能力を出せない場合は未達として記録する。WinSCard/PCSCはnative stackを別経路で確認。 |
| 必要に応じて | LNB電圧実測と非対応機種の拒否、複数チューナー列挙/daemon並列、変更機能に応じた機種×OS/ABIの追加確認。 | W3U3はLNB対応。W3U2/W3U3/V2以外はREADMEの能力表で確認し、非対応機種へsoftware要求を送らない。 |
| 初回/major | 必要時項目を通し、代表機で全無料channelを各10秒TS取得。macOS/Windows各30分連続、残るOS/ABI各10分連続で取得する。全channel取得は代表機だけ。別途指示されたソークは別記する。 | 現地channel planとmodel receiver割当を記録。Windows ZIPは通常aggregate外でも同候補の`win-candidate`を使う。 |

全配布archiveはnative OS/ABIで回帰し、代表機の全channel10秒取得を全archiveへ広げない。具体的な候補・操作・判定は以下の手順、実施結果は[`release-validation/1.0.0.md`](release-validation/1.0.0.md)と初回機種記録へ分ける。

## 1. 候補と配布物

候補source commitの `build_userland.yml` によるcandidate artifactを使い、run/ref/SHA、artifact ID、全archive hash、source manifest、license/provenanceを保存する。通常release配布binaryはLinux glibc/musl x86_64/aarch64、macOS arm64、Android aarch64/armv7a/x86_64の8 target。Windows ZIPは通常release aggregateに接続されない。Windowsが該当版の検証対象なら同候補runの `win-candidate` artifactを使う。source archiveも検査する。空の作業directoryへ展開し、同梱checksumを確認する。

```sh
mkdir -p candidate
gh run download <candidate-run-id> --name release-candidate --dir candidate
(cd candidate && sha256sum -c SHA256SUMS)
```

通常release aggregateに含まれないWindows ZIPは配布物として数えない。適用版にWindows native検証が含まれる場合はこの区別を保ち、通常配布物にないことを理由にnative確認を除外しない。欠落・hash不一致・source/licenseの不整合は候補失敗。

## 2. device-free gate

候補commitの全CI build/test/package/audit job成功を確認する。README記載のnative・cross build、unit test、sanitizer、package展開試験、source archive再buildを行い、出力と終了値を保存する。Linux/macOSで `asicend --version`、`asicen-ts --version`、`asicenctl --version`、`asicend --list-json` を実行し、版情報と妥当なinventory JSONを確認する。USBなしinventoryはscan incomplete等の状態を区別して示す。`--list-json` はdevice ownership不要。異常引数・未知optionのexitとstderrをREADMEのCLI契約に照合する。

## 3. archiveごとの起動と受信

毎release、代表Linux x86_64機で2時間連続受信し、全配布archiveをnative OS/ABIで短時間回帰する。初回/major追加確認ではmacOS/Windowsは各30分、残るOS/ABIは各10分連続受信し、全無料channel各10秒は代表機だけで行う。Windowsが対象の版ではnative確認に同source candidateの `win-candidate` artifactを使い、通常配布binaryの代替とは扱わない。

候補archiveをnative hostに展開し、`asicend --list-json` でUSB位置とmodelを確認する。値は現地の `BUS-PORT`、firmware、runtimeへ置換する。USB複数機能機種では確認済み内部hubで相方が一意な場合以外、両positionを指定する。

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

hardware pathは `W3u3CardMailboxHardware` によるmodel-specific mailbox adapterである。過去のphysical CARD remove/reinsert記録はPX-W3U3上のlinux-glibc-x86_64候補であり、他model/OSの認定を意味しない。Linux/macOSの配布IFDをnative OSのPC/SC stackへ設定し、reader/card列挙、reset/ATR、通常APDU交換まで別経路で確認する。AndroidにはIFDなし。Windows WinSCardは該当版で検証対象の場合、CLIのCARD_*とは別経路として確認する。各OS/platform routeの未実施を未確認として残す。

USBはdaemon idleとTS受信中の両方で対象deviceを抜く。stream clientはUSB切断exit 7を確認し、戻した同一portの再列挙、旧daemonのcleanup、新daemonでのstatus/list、再受信を順に行う。自動hotplug復旧を期待しない。Termuxでは `termux-usb -l` と `termux-usb -r <usb-path>` で許可を取り、再列挙後にFDを再取得する。BUSYなら強制切断しない。

### Windows native / WinSCard

Windows x86_64は回帰対象である。通常release aggregateにWindows ZIPがない場合は同じcandidate source runの`win-candidate` artifactを展開し、hash/manifest/provenanceを照合する。native Windows hostでUSB driverを自動変更せず、既に許可済みのlibusb access pathを記録する。実時間は共通頻度表に従い、初回/majorの30分連続受信は全無料channel取得とは別に記録する。

```powershell
$D = (Resolve-Path .\asicend.exe).Path
$C = (Resolve-Path .\asicenctl.exe).Path
$B = (Resolve-Path .\asicen-ts.exe).Path
$FW = (Resolve-Path .\firmware\<model-firmware>).Path
$RT = Join-Path $env:TEMP "asicen-rc-runtime-$PID"
New-Item -ItemType Directory -Path $RT | Out-Null
& $D --list-json
$selector = 'usb:BUS-PORT[,BUS-PORT]'
& $D --device $selector --instance rc --firmware $FW --runtime-dir $RT
# daemonを別PowerShellで維持し、同一instance/runtimeから照会・受信する
& $C --instance rc --runtime-dir $RT status
& $C --instance rc --runtime-dir $RT list
& $B --instance rc --runtime-dir $RT --receiver <ID> --channel T27 --time <SECONDS> --output t27.ts
```

candidate内のapplication-local `WinSCard.dll`をWindows native PC/SC consumerからロードし、context、reader/card status、connect、ATR/reset、反復APDU、disconnect/context releaseを確認する。物理CARD抜去時の不在と再挿入後の復帰も記録する。CLIの`card-*`結果はWinSCard経路の代用にならない。生ATR、card ID、APDU応答本文は公開記録へ保存しない。

## 5. LNB・soak・cleanup

PX-W3U2/W3U3/V2などREADMEで15V対応が明示された機種のみ、壁設備と他給電源を分離し、衛星端子へテスターを接続する。停止状態から `--allow-lnb-power` を付けてdaemonを起動し、`asicenctl --runtime-dir "$RT" --instance rc lnb-on` 後に実測15V、`asicenctl --runtime-dir "$RT" --instance rc lnb-off` 後に要求0と実測0Vを確認する。独立要求と受信部 `--lnb-voltage` は別の状態として記録する。非対応機はsoftware要求も試さない。daemon停止後0Vと通常配線への復帰を確認する。

Soakでは代表Linux x86_64機の2時間連続受信を実施する。開始・定期・終了時にTS/counters、process/FD/RSS、channel、終了値を記録する。終了後はclient/daemonを正常停止し、status/process/runtime socketを確認して後始末する。候補SHA/archive hash、host/OS/arch/model、firmware、channel、操作、実コマンド、期待/実測、エラー、未確認事項をrelease recordへ保存する。全gateを終えた後にだけ結果文書をcommitし、code/build inputsが候補と同一であることを確認する。正式tag/releaseは別途の承認対象。

## 6. 製品固有の適用範囲

Windowsの配布経路は `win-candidate` が通常release aggregateへ接続されていない点を踏まえる。LNBは対応3機種のみ、receiver/T-S割当と再選局可能性は機種別README表に従う。カードや給電機能がないprofileに試験を要求せず、各claimを `今回再検証` / `継承` / `未認定` / `対象外` で記録する。
