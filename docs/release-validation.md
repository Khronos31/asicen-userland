# Stable リリース前検証手順

この文書はASICEN Stable releaseで再利用する準備・操作・判定・cleanupの正本です。版固有matrixと検証記録は [`release-validation/1.0.0.md`](release-validation/1.0.0.md) のように版別文書へ記録します。公開契約は [`SPEC.md`](../SPEC.md)、CLI構文・機種能力は [`README.md`](../README.md) と各コマンドの `--help` が正です。

## 共通の検証項目と頻度

用語は4製品で共通とする。代表機はLinux x86_64 glibc、機種はチューナー、製品はドライバ、ソークは30分以上の連続TS取得を指す。ASICENの通常の無指定時検証機種はPX-W3U3だが、機種初回の対象を限定しない。ASICENが対象とする全機種で初回手順を個別に実施し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ機種別に記録する。

| 区分 | 適用頻度 | ASICENでの確認 |
|---|---|---|
| 機種初回 | ASICENの全機種で、機種ごとに代表機の2時間連続受信と毎release項目を実施する。必要時項目も通し、結果は[`release-validation/initial-model-checks.md`](release-validation/initial-model-checks.md)へ機種別に記録する。 | PX-W3U2とPX-W3U3は4番組同時録画、地上波/衛星、CARD、対応LNBを確認する。ほかの機種はREADME記載の機種別能力に従う。 |
| 回帰（毎release） | 代表機2時間連続受信。全配布archiveの各OS/ABIで短時間実機回帰、`--list-json`、機種別の同時受信能力とTS/counter/終了値を確認する。最大試験で地上/衛星3波が不足する場合は不足分を短時間取得する。受信中status、反復APDU、正常停止とprocess/FD/IPC残留なし、CARD挿抜、USB抜去/再挿入を確認する。Windows/WinSCard・Linux/macOS PC/SC経路も各対象OSで行う。 | PX-W3U2とPX-W3U3は4番組を同時録画し、各TS/counter/終了値を確認する。W3U3のtuner path数、discovery route数、公開slot数は録画数と別々に記録する。WinSCard/PCSCはnative stackを別経路で確認する。 |
| 必要に応じて | LNB電圧実測と非対応機種の拒否、複数チューナー列挙/daemon並列、変更機能に応じた機種×OS/ABIの追加確認。 | W3U3はLNB対応。W3U2/W3U3/V2以外はREADMEの能力表で確認し、非対応機種へsoftware要求を送らない。 |
| 初回/major | 必要時項目を通し、代表機で全無料channelを各10秒TS取得。macOS/Windows各30分連続、残るOS/ABI各10分連続で取得する。全channel取得は代表機だけ。別途指示されたソークは別記する。 | 現地channel planとmodel receiver割当を記録。Windows ZIPは通常aggregate外でも同候補の`win-candidate`を使う。 |

全配布archiveはnative OS/ABIで回帰し、代表機の全channel10秒取得を全archiveへ広げない。具体的な候補・操作・判定は以下の手順、実施結果は[`release-validation/1.0.0.md`](release-validation/1.0.0.md)と初回機種記録へ分ける。

## 共通実行環境表

表の列は4製品で共通とする。配布targetごとに対応するnative OS/ABIで実行し、依存・driver/permission・firmware/input・CARD backendを記録する。実際に使ったhost OS/ABIと結果は版別の検証記録へ記す。

| ID | OS / ABI | 依存 / driver / permission | firmware / input | CARD backend | 配布artifact / 手順 |
|---|---|---|---|---|---|
| E03 | AnduinOS Linux x86_64 / glibc | native CPU、libusb静的、USB access pathを記録 | 共通loader image | Linux PC/SC IFDを別途確認 | `linux-glibc-x86_64` / §3–§5 |
| E02 | HAOS Alpine Linux x86_64 / musl | native CPU、libusb静的、Supervisor USB access pathを記録 | 共通loader image | musl用IFDを別途確認 | `linux-musl-x86_64` / §3–§5 |
| E15-G | native Linux aarch64 / glibc | native aarch64、libusb静的、USB access pathを記録 | 共通loader image | Linux PC/SC IFDを別途確認 | `linux-glibc-aarch64` / §3–§5 |
| E15-M | native Linux aarch64 / musl | native aarch64、emulationなし。Alpine arm64 runtime | 共通loader image | musl用IFDを別途確認 | `linux-musl-aarch64` / §3–§5 |
| E04 | macOS arm64 | libusb静的、USB access permissionを記録 | 共通loader image | macOS PC/SC IFDを別途確認 | `darwin-arm64` / §3–§5 |
| E05 | Android Termux aarch64 | Termux:API / `termux-usb`、再列挙後FD再取得 | 共通loader image | 非対応 | `android-aarch64` / §3–§5 |
| E06 | Android Termux armv7a | Termux:API / `termux-usb`、再列挙後FD再取得 | 共通loader image | 非対応 | `android-armv7a` / §3–§5 |
| E07 | Android Termux x86_64 | Termux:API / `termux-usb`、再列挙後FD再取得 | 共通loader image | 非対応 | `android-x86_64` / §3–§5 |
| W1 | Windows 11 x86_64 | 使用許可済みlibusb driver。変更しない | 同source candidateの`win-candidate` | native WinSCardを別途確認 | Windows候補ZIP / §3–§5 |

## 共通試験シナリオ

各シナリオは同じ順に「適用頻度/対象能力」「準備/起動」「操作/利用者への依頼」「期待値」「停止/cleanup」「記録」を記載する。チャンネル、USB位置、firmware pathは現地の値へ置き換え、失敗試行も保存する。HAOS/SCSでは各物理操作を依頼する直前にCodexは`beep`、Claude Codeは`vibe`を実行し、依頼から最大5分待つ。期限内に操作がなければ、その工程を未完了として停止する。

### A. 列挙と同時録画

| 適用頻度 / 対象能力 | 準備 / 起動 | 操作 / 利用者への依頼 | 期待値 | 停止 / cleanup | 記録 |
|---|---|---|---|---|---|
| 毎release。PX-W3U2/W3U3は4番組同時、他機種はREADMEのreceiver能力 | `asicend --list-json`で機種/USB pathを固定し、候補firmwareと空の`$RT`を用意。`asicend --device usb:<BUS-PORT[,BUS-PORT]> --instance rc --firmware <FIRMWARE> --runtime-dir "$RT"`を起動 | `status`/`list`から使用receiverを選び、W3U2/W3U3では地上波2番組と衛星2番組を同時開始する。下記の4 client例を使用する | 全4 clientが同時に稼働し、各TS同期/品質、packet・sync・TEI・continuity counter、各client終了0を確認。順次取得で代用しない | 4 clientを通常終了し、daemonを通常終了。status、process、FD、IPC/runtime残留がないことを確認 | 候補/source SHA、model/revision、channel/receiver、各CLIとexit、TS/counter、cleanupを記録 |

```sh
asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R1" --channel T27 --time <SECONDS> --output terrestrial-1.ts & P1=$!
asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R2" --channel T30 --time <SECONDS> --output terrestrial-2.ts & P2=$!
asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R3" --channel BS01_0 --lnb-voltage 0 --time <SECONDS> --output satellite-1.ts & P3=$!
asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R4" --channel BS15_0 --lnb-voltage 0 --time <SECONDS> --output satellite-2.ts & P4=$!
if wait "$P1"; then RC1=0; else RC1=$?; fi
if wait "$P2"; then RC2=0; else RC2=$?; fi
if wait "$P3"; then RC3=0; else RC3=$?; fi
if wait "$P4"; then RC4=0; else RC4=$?; fi
```

`R1`–`R4`はcandidateで起動した`asicenctl list`の実際のreceiver IDから設定し、重複しない4系統を用いる。`T27`/`T30`/`BS01_0`/`BS15_0`は例であり、現地channel planと各receiverの公開system能力に合わせて選び、実値を記録する。各client開始時刻と終了時刻が重なることを確認し、4本全ての終了値を保存する。

各4 clientが動作している間、別terminalからstatus確認と通常CARD APDU 10回を実施する。CARDなしprofileではAPDU項目を対象外とし、受信中statusは各対象で記録する。

```sh
asicenctl --runtime-dir "$RT" --instance rc status
asicenctl --runtime-dir "$RT" --instance rc list
for i in 1 2 3 4 5 6 7 8 9 10; do
  if asicenctl --runtime-dir "$RT" --instance rc card-apdu 90:30:00:00:00 > "$LOG/stream-card-$i.apdu"; then rc=0; else rc=$?; fi
  echo "$rc" > "$LOG/stream-card-$i.rc"
done
```

各status/APDUは4 clientの開始/終了時刻内に行い、APDUはCoreの単発送信を10回個別に呼び出す。clientごとのTS/counter/exitとは別に照会時刻・response class・各終了値を保存する。

### B. CARD抜去・再挿入

適用: readerと内蔵CARD経路を備える機種。受信を止めてもdaemonは起動したままにし、各人手操作の前にIssue/作業画面で抜去または再挿入を依頼する。依頼後最大5分待ち、応答がなければその工程を未完了で終了する。

```sh
asicenctl --runtime-dir "$RT" --instance rc card-status
asicenctl --runtime-dir "$RT" --instance rc card-atr
asicenctl --runtime-dir "$RT" --instance rc card-reset
for i in 1 2 3 4 5 6 7 8 9 10; do
  if asicenctl --runtime-dir "$RT" --instance rc card-apdu 90:30:00:00:00 > "$LOG/card-$i.apdu"; then rc=0; else rc=$?; fi
  echo "$rc" > "$LOG/card-$i.rc"
done
```

1. 準備時にstatus present、ATR取得、reset後ATRと10回のAPDU終了値を確認する。
2. 親が「対象機のカードを抜いてください」と依頼し、最大5分待つ。抜去後に`card-status`で不在、`card-atr`と`card-apdu`で`CARD_REMOVED`/`NO_CARD`・exit 9を確認する。

```sh
asicenctl --runtime-dir "$RT" --instance rc card-status
asicenctl --runtime-dir "$RT" --instance rc card-atr
asicenctl --runtime-dir "$RT" --instance rc card-apdu 90:30:00:00:00
```

3. 親が「同じカードを再挿入してください」と依頼し、最大5分待つ。再挿入後にstatus present、ATR/reset/APDUの復帰を確認する。

```sh
asicenctl --runtime-dir "$RT" --instance rc card-status
asicenctl --runtime-dir "$RT" --instance rc card-atr
asicenctl --runtime-dir "$RT" --instance rc card-reset
asicenctl --runtime-dir "$RT" --instance rc card-apdu 90:30:00:00:00
```

4. CARD操作を終了しdaemonを通常停止する。生ATR、カードID、APDU応答本文は公開記録へ残さない。

期待値は不在時の明示的失敗と、再挿入後の経路復帰。readerなし/unsupported profileの結果はprofile単位で記録する。Linux/macOS PC/SC IFDとWindows WinSCardは各native stackで別シナリオとして同様の挿抜を行い、CLI結果で代用しない。

### C. USB切断・再接続

| 適用頻度 / 対象能力 | 準備 / 起動 | 操作 / 利用者への依頼 | 期待値 | 停止 / cleanup | 記録 |
|---|---|---|---|---|---|
| 毎release。対応する各USB接続機種のidle時とstreaming時 | `asicend --list-json`でportを記録し、idle確認後、別runで短時間clientを起動 | 親がidle時と受信中それぞれで対象USBを抜くよう依頼。client/daemonを観測。親が同じportへの再挿入を依頼し、再列挙後に新daemonを起動 | 切断表示と規定終了値を記録し、ハングやclient残留なし。自動hotplug再取得は期待しない。新daemonのstatus/listと短時間再取得が成功 | client/旧daemonを通常停止し終了を待つ。新daemonを通常停止しFD/IPC/runtime残留を確認。Termuxは許可を再取得し新FDで起動 | 物理操作時刻、port、ログ、exit、再列挙/再取得、permission経路とcleanup |

### D. cleanupと版別結果

受信clientを通常終了しdaemonを通常停止する。process/FD/IPC/runtime endpointを確認して後始末する。環境表と頻度表の実施結果は版別文書へ、実測済み/未実施/失敗/対象外を分けて記録する。

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

専用pcsc-lite prefixとその`libpcsclite.so.1`を使い、reader config directoryだけを登録してforeground daemonを起動する。既存のLinux probeは次の形式で実行する。

```sh
LD_LIBRARY_PATH=<isolated-pcsc-lite-prefix>/lib \
  <isolated-pcsc-lite-prefix>/sbin/pcscd --foreground \
  --config <candidate-reader-config-dir>

LD_LIBRARY_PATH=<isolated-pcsc-lite-prefix>/lib pcsc_scan -r
set -o pipefail
LD_LIBRARY_PATH=<isolated-pcsc-lite-prefix>/lib pcsc_scan -c 2>&1 | \
  sed -E 's/(ATR[[:space:]]*[:=]).*/\1 <redacted>/'
LD_LIBRARY_PATH=<isolated-pcsc-lite-prefix>/lib \
  <existing-bcas-safe-probe>
```

`pcsc_scan -r`はexit 0かつASICEN readerが1件、ATRを伏せた`pcsc_scan -c`はpipeline exit 0かつcard insertedを期待する。既存`bcas-safe-probe`はPC/SC connect後の標準B-CAS初期設定、`get_init_status()`、read-only `get_id()`を行い、exit 0かつ`bcas_init=ok` / `init_status=ok` / `id_query=ok`を期待する。readerを1つだけ登録し、probeのreader選択を一意にする。カード不在・ATR/resetの独立確認は同じdaemon/runtime/instanceの`asicenctl card-status`, `card-atr`, `card-reset`を使い、ATR/resetのraw出力は`/dev/null`へ送り終了値だけを記録する。CLI CARD操作はnative PC/SC probe結果の代用ではない。raw ATR、card ID、status内識別値、APDU response bytesを出力・保存しない。probeがtest hostにない場合は新規probeを作らず、そのPC/SC transaction gateを未実施とする。consumerを終了したらisolated `pcscd`だけをSIGINTで停止し、process/socket消失を確認してから`asicend`を通常停止する。test config/runtimeを削除し、system serviceは変更しない。

macOSでもnative `.bundle`を使うが、system PC/SC serviceをreload/restartしてcandidate IFDを加える操作はしない。既存のisolated native stackとnative `pcsc_scan` / `bcas-safe-probe`が用意された専用test hostに限り、`pcsc_scan -r`, `pcsc_scan -c`とsafe probeを同様に実行する。native consumerや隔離stackが用意されていなければ実施不能として記録し、Linux結果を代用しない。AndroidにはIFDがない。

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

candidate内のapplication-local `WinSCard.dll`はconsumerと同じdirectoryに置き、consumerがこのabsolute pathをloadする。既存のWindows scratch checkは`LoadLibraryW`と`GetProcAddress`で`SCardEstablishContext` / `SCardTransmit`等のexportを確認するDLL load/export smokeであり、reader列挙・context確立・connect・ATR/reset・APDUのconsumer試験ではない。現在の`win-candidate`にはnative transaction consumer/API probeが含まれず、repoにも実行手順付きの既存probeがないため、このgateは実施不能として未完了に記録する。native consumerを用意できないまま新しいprobeを作成せず、CI、DLL export smoke、ASICEN CLIの`card-*`をnative WinSCard transactionの合格にしない。既存のnative consumerが候補とともに提供された場合だけ、その実行file・loaded absolute DLL path・reader選択・status/connect/ATR/reset/標準APDU/disconnect/context release・終了値・cleanupを記録する。生ATR、card ID、APDU response本文は公開記録へ保存しない。

## 5. LNB・soak・cleanup

PX-W3U2/W3U3/V2などREADMEで15V対応が明示された機種のみ、壁設備と他給電源を分離し、衛星端子へテスターを接続する。停止状態から `--allow-lnb-power` を付けてdaemonを起動し、`asicenctl --runtime-dir "$RT" --instance rc lnb-on` 後に実測15V、`asicenctl --runtime-dir "$RT" --instance rc lnb-off` 後に要求0と実測0Vを確認する。独立要求と受信部 `--lnb-voltage` は別の状態として記録する。非対応機はsoftware要求も試さない。daemon停止後0Vと通常配線への復帰を確認する。

Soakでは代表Linux x86_64機の2時間連続受信を実施する。開始・定期・終了時にTS/counters、process/FD/RSS、channel、終了値を記録する。終了後はclient/daemonを正常停止し、status/process/runtime socketを確認して後始末する。候補SHA/archive hash、host/OS/arch/model、firmware、channel、操作、実コマンド、期待/実測、エラー、未確認事項をrelease recordへ保存する。全gateを終えた後にだけ結果文書をcommitし、code/build inputsが候補と同一であることを確認する。正式tag/releaseは別途の承認対象。

## 6. 製品固有の適用範囲

Windowsの配布経路は `win-candidate` が通常release aggregateへ接続されていない点を踏まえる。LNBは対応3機種のみ、receiver/T-S割当と再選局可能性は機種別README表に従う。カードや給電機能がないprofileに試験を要求せず、各claimを `今回再検証` / `継承` / `未認定` / `対象外` で記録する。
