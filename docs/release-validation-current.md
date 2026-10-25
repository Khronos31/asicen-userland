# 現行 Stable 候補のリリース前回帰手順

この文書は現行配布構成の候補ごとの再現手順です。過去候補の行列・soak時間・実測は今回の合格を意味しません。公開契約は [`SPEC.md`](../SPEC.md)、CLI構文・機種能力は [`README.md`](../README.md) と各コマンドの `--help` が正です。過去の候補記録は [`release-validation.md`](release-validation.md) に残します。

## 1. 候補と配布物

候補source commitの `build_userland.yml` によるcandidate artifactを使い、run/ref/SHA、artifact ID、全archive hash、source manifest、license/provenanceを保存する。配布binaryはLinux glibc/musl x86_64/aarch64、macOS arm64、Android aarch64/armv7a/x86_64の8 target。WindowsはCIでbuild/testするが現行配布外。source archiveも検査する。空の作業directoryへ展開し、同梱checksumを確認する。

```sh
mkdir candidate && cd candidate
tar -xf <candidate-artifact.tar.gz>
sha256sum -c SHA256SUMS
```

Windows CI-only ZIPは配布認定に数えない。欠落・hash不一致・source/licenseの不整合は候補失敗。

## 2. device-free gate

候補commitの全CI build/test/package/audit job成功を確認する。README記載のnative・cross build、unit test、sanitizer、package展開試験、source archive再buildを行い、出力と終了値を保存する。Linux/macOSで `asicend --version`、`asicen-ts --version`、`asicenctl --version`、`asicend --list-json` を実行し、版情報と妥当なinventory JSONを確認する。USBなしinventoryはscan incomplete等の状態を区別して示す。`--list-json` はdevice ownership不要。異常引数・未知optionのexitとstderrをREADMEのCLI契約に照合する。

## 3. archiveごとの起動と受信

最低soak時間はLinux x86_64代表機2時間、macOS arm64 30分、Windows x86_64 30分、その他の全配布binary各10分。2時間は製品各機種が一度以上、かつreleaseごと代表Linux x86_64機。全配布binaryは当該archiveをnative OS/ABIで実機確認し、major versionでは各OS/archの短時間TSを行う。Windows buildは現行配布binaryの代替にしない。

候補archiveをnative hostに展開し、`asicend --list-json` でUSB位置とmodelを確認する。値は現地の `BUS-PORT`、firmware、runtimeへ置換する。USB複数機能機種では確認済み内部hubで相方が一意な場合以外、両positionを指定する。

```sh
RT=$(mktemp -d "${TMPDIR:-/tmp}/asicen-release.XXXXXX")
asicend --device usb:<BUS-PORT[,BUS-PORT]> --instance rc --firmware <FIRMWARE> --runtime-dir "$RT" & P=$!
asicenctl --instance rc --runtime-dir "$RT" status
asicenctl --instance rc --runtime-dir "$RT" list
asicen-ts --instance rc --receiver 1 --channel T27 --packet-count 30000 --output t27.ts
```

statusがready、listにreceiverが存在すること、client終了値0、TSが188-byte packet列として同期すること、daemonのpacket/sync/TEI/continuity統計を確認する。各実地域の地上波全chと無料衛星全対象chを、それぞれ10秒ずつ取得する。チャンネル名・周波数・TSID等は現地channel planから列挙し、ログへ実値を記録する。衛星は実機のreceiver割当とREADMEに従い `--channel BS01_0 --lnb-voltage 0` を使う。無料CS raw TSも含める。出力非空だけをpassとせず、README/SPECのTS基準で判定し、失敗試行を消さない。

受信停止後、同じdaemon/runtimeで同じ受信を再取得し、normal exitと再取得成功を確認する。機種差があるので使うreceiverとT/Sの排他条件を記録する。

## 4. CARD・PC/SC・USB

CARDを搭載した対象機で、daemonを起動したまま受信を止め、以下を各段階で実行する。

```sh
asicenctl --runtime-dir "$RT" --instance rc card-status
asicenctl --runtime-dir "$RT" --instance rc card-atr
```

reader status/ATR probeの実測終了値を保存する。制御CLIのreset/APDUは未実装操作として失敗する契約なので、成功を要求しない。カード抜去を依頼してからstatus/ATRを再実行し、不在状態とエラーを確認する。再挿入後にstatus/ATRが戻ることを確認する。Linux/macOS PC/SC IFDとWindows WinSCard経路は実装・配布対象のnative OSで分け、OSの標準PC/SC APIからreader列挙とATR取得まで確認する。APDU/reset機能は未実装として記録する。生ATR、カードID、応答本文は公開記録に保存しない。非対応profileは対象外と明記する。

USBはdaemon idleとTS受信中の両方で対象deviceを抜く。stream clientはUSB切断exit 7を確認し、戻した同一portの再列挙、旧daemonのcleanup、新daemonでのstatus/list、再受信を順に行う。自動hotplug復旧を期待しない。Termuxでは `termux-usb -l` と `termux-usb -r <usb-path>` で許可を取り、再列挙後にFDを再取得する。BUSYなら強制切断しない。

## 5. LNB・soak・cleanup

PX-W3U2/W3U3/V2などREADMEで15V対応が明示された機種のみ、壁設備と他給電源を分離し、衛星端子へテスターを接続する。停止状態から `--allow-lnb-power` を付けてdaemonを起動し、`asicenctl --runtime-dir "$RT" --instance rc --device usb:<BUS-PORT> lnb-on` 後に実測15V、`asicenctl --runtime-dir "$RT" --instance rc --device usb:<BUS-PORT> lnb-off` 後に要求0と実測0Vを確認する。独立要求と受信部 `--lnb-voltage` は別の状態として記録する。非対応機はsoftware要求も試さない。daemon停止後0Vと通常配線への復帰を確認する。

soakはarchive別のnative OS/ABIで指定時間連続受信する。開始・定期・終了時にTS/counters、process/FD/RSS、channel、終了値を記録する。地域全ch/無料衛星全対象の10秒回帰とは別に実施する。終了後はclient/daemonを正常停止し、status/process/runtime socketを確認して後始末する。候補SHA/archive hash、host/OS/arch/model、firmware、channel、操作、実コマンド、期待/実測、エラー、未確認事項をrelease recordへ保存する。全gateを終えた後にだけ結果文書をcommitし、code/build inputsが候補と同一であることを確認する。正式tag/releaseは別途の承認対象。

## 6. 製品固有の適用範囲

Windowsは現在の配布対象外だが、既存Windows workflowのCI結果は確認する。LNBは対応3機種のみ、receiver/T-S割当と再選局可能性は機種別README表に従う。カードや給電機能がないprofileに試験を要求せず、各claimを `今回再検証` / `継承` / `未認定` / `対象外` で記録する。
