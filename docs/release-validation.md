# Stable リリース前検証手順

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
| 配布artifact / 手順 | 対象archive識別子とこの正本内の該当手順 | artifact hash、host、試験結果 | `artifact: <target>; procedure: [archive別手順](#3-release-archive-procedure)` の順 |

共通IDは E02=Linux/x86_64/musl、E03=Linux/x86_64/glibc、E04=macOS/arm64、E05=Android/aarch64、E06=Android/armv7a、E07=Android/x86_64、E15-G=Linux/aarch64/glibc、E15-M=Linux/aarch64/musl、E17=Windows 11/x86_64を表す。IDはhost名や試験済み状態を表さない。

| ID | OS / ABI | 依存 / driver / permission | firmware / 補助data | RF試験入力（system / channel） | 製品CLIからの内蔵CARD操作 | native PC/SC/WinSCard adapter | 配布artifact / 手順 |
|---|---|---|---|---|---|---|---|
| E03 | Linux / x86_64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-glibc-x86_64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E02 | Linux / x86_64 / musl | libusb: 静的1.0.30; USB access: HAOS Supervisorの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-musl-x86_64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E15-G | Linux / aarch64 / glibc | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-glibc-aarch64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E15-M | Linux / aarch64 / musl | libusb: 静的1.0.30; USB access: native OSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Linux PC/SC: libc対応IFD | artifact: `linux-musl-aarch64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E04 | macOS / arm64 / Apple system runtime | libusb: 静的1.0.30; USB access: macOSの許可済み経路 | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | macOS PC/SC: bundle | artifact: `darwin-arm64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E05 | Android (Termux) / aarch64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; FD handoff: daemonへ2 FD | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Android: なし | artifact: `android-aarch64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E06 | Android (Termux) / armv7a / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; FD handoff: daemonへ2 FD | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Android: なし | artifact: `android-armv7a`; procedure: [archive別手順](#3-release-archive-procedure) |
| E07 | Android (Termux) / x86_64 / Android NDK ABI | libusb: 静的1.0.30; USB access: Termux USB permission; FD handoff: daemonへ2 FD | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Android: なし | artifact: `android-x86_64`; procedure: [archive別手順](#3-release-archive-procedure) |
| E17 | Windows 11 / x86_64 / Win32 | libusb: 静的1.0.30; USB access: 既存許可済みlibusb driver | firmware: loader image; 補助data: なし | system: 対応modelのISDB-T/ISDB-S; channel: 現地channel planから対応channelを選択 | CARD status/ATR/reset/APDU CLI; 操作対応はmodel依存 | Windows WinSCard: application-local DLL | artifact: `win-candidate`; procedure: [archive別手順](#3-release-archive-procedure) |

## 共通試験シナリオ T00–T14

各IDを同じ順序・同じ6項目で実施する。$RT/$LOG/$BUS-PORT/$CHANNEL_PLANはrunごとの実値に置換し、空のcandidate専用runtime/logを使う。物理操作を依頼する各回は既定通知を行い、最大5分で応答がなければ未完了として停止する。OS/API固有の長い操作は同じT-IDから後続recipeを参照する。

### T00 候補/配布物固定

- **適用対象/頻度:** 毎releaseのcandidate作成時。
- **準備:** source SHA、CI run/attempt、archive targetとhashを固定。POSIX git rev-parse HEAD/sha256sum、Windows Get-FileHashを保存。
- **実操作/利用者への依頼:** 固定したsource commitとCI runから候補archiveを取得し、POSIXではsha256sum -c SHA256SUMS、WindowsではGet-FileHashでarchive hashを照合する。archiveのmanifest/source refがcandidate commit/runと一致することを確認する。
- **期待値/終了値:** provenance/hash一致の候補だけ次へ進む。候補不在は未完了。
- **停止/cleanup:** deviceを開かず、archive/logを保持。
- **記録:** source SHA、CI run/attempt、artifact ID/target/hash、取得日時、未取得理由。

### T01 環境/permission/input

- **適用対象/頻度:** 全native archive。初回は全modelごと。
- **準備:** E-ID/OS/ABI、USB access、firmware/model data、channel planを環境表と照合。
- **実操作/利用者への依頼:** Linux uname -a/uname -m/ldd --version、macOS sw_vers/uname -m、Android getprop/uname -m、Windows PowerShell OS/architectureを記録。
- **期待値/終了値:** OS/ABIがarchiveに一致し、アクセス許可/入力が揃う。不一致はdevice操作前に止める。
- **停止/cleanup:** 権限不足やdriver変更が必要なら操作せず未実施記録。
- **記録:** E-ID/host/OS/ABI/model、USB topology/permission、firmware/data、channel plan。

### T02 列挙/selector/ready

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

    CAPTURE_SECONDS=10
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R1" --channel T27 --time "$CAPTURE_SECONDS" --output "$LOG/terrestrial-1.ts" & P1=$!
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R2" --channel T30 --time "$CAPTURE_SECONDS" --output "$LOG/terrestrial-2.ts" & P2=$!
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R3" --channel BS01_0 --lnb-voltage 0 --time "$CAPTURE_SECONDS" --output "$LOG/satellite-1.ts" & P3=$!
    asicen-ts --instance rc --runtime-dir "$RT" --receiver "$R4" --channel BS15_0 --lnb-voltage 0 --time "$CAPTURE_SECONDS" --output "$LOG/satellite-2.ts" & P4=$!
    for P in "$P1" "$P2" "$P3" "$P4"; do if wait "$P"; then RC=0; else RC=$?; fi; printf "%s %s\n" "$P" "$RC"; done
- **期待値/終了値:** 各clientの稼働時間が重なり、各exit 0とTS/counter合格。順次取得を同時試験の代用にしない。
- **停止/cleanup:** 全client通常終了後daemon通常停止、残留なし。
- **記録:** model/receiver/system/channel、各start/end/exit、TS/counter、cleanup。

### T04 不足波/全無料channel

- **適用対象/頻度:** 毎releaseは不足波。初回/majorは全機種の代表機で全無料channel各10秒。
- **準備:** 現地channel planとmodel能力から対象system/channel一覧を作る。
- **実操作/利用者への依頼:** asicen-ts --instance rc --runtime-dir "$RT" --receiver <ID> --channel <CHANNEL> --time 10 --output "$LOG/<CHANNEL>.ts"を各channelで実行。
- **期待値/終了値:** 各対象channelのTS/exit合格。未取得は未完了、非対応波だけ根拠付きN/A。
- **停止/cleanup:** client wait/exit保存、daemon通常停止。
- **記録:** model/system/channel/全channel list、TS解析/counter/exit、未取得理由。

### T05 stream中monitor/control

- **適用対象/頻度:** 毎release。CARD/controlは製品能力に従う。
- **準備:** capture clientを起動し、別terminalでstatus/list/controlを行えるようにする。
- **実操作/利用者への依頼:** 受信中に別terminalからasicenctl --instance rc --runtime-dir "$RT" statusとlistを実行し、対応modelでは同CLIのcard-apdu 90:30:00:00:00を10回個別送信して各終了値を保存する。
- **期待値/終了値:** 照会/control時刻がstreamと対応し各response/exitを保存。機能外操作はN/A。
- **停止/cleanup:** client/daemonを通常停止し残留確認。
- **記録:** 照会/control時刻、response class、各exit、streamとの重なり。

### T06 CARD物理挿抜/復帰

- **適用対象/頻度:** CARD reader/status対応model。毎release。
- **準備:** capture停止・daemon維持。初期CARD状態とreader有無を確認。
- **実操作/利用者への依頼:** asicenctl card-status/card-atr/card-reset/card-apduの初期値を採取。抜去依頼/最大5分待ち後同CLI、再挿入を別依頼して再照会。
- **期待値/終了値:** 抜去後statusはabsent、ATR/APDUはCARD_REMOVEDまたはNO_CARD (exit 9)。再挿入後status/ATR/reset/APDUが復帰し、各通常操作はexit 0。CARD対応readerがないmodelは理由付きN/A。応答なしは未完了。raw ATR等は保存しない。
- **停止/cleanup:** daemon通常停止、FD/IPC/runtimeを確認。
- **記録:** 依頼時刻、状態/response/exit、復帰、cleanup。

### T07 USB idle/stream切断と再取得

- **適用対象/頻度:** 毎release。USB対応deviceのidle/streamを別run。
- **準備:** USB pathをlist-jsonで固定。有限captureとそれより短い観測窓を準備。
- **実操作/利用者への依頼:** `asicend --list-json`でUSB pathを特定し、streamなしのdaemonを起動してstatus/listとdaemonのUSB owner状態を記録する。daemonがdeviceを保持したidle中に利用者へ抜去を依頼し、応答後にstatus/list、診断、daemonの実際の終了値を採取する。通常停止後、再接続を依頼し、再列挙したpathで新runtime/daemonを起動してstatus/listと短い`asicen-ts`を実行する。別runでは有限`asicen-ts`中に抜去を依頼し、client/daemonの切断診断と終了値を採取する。
- **期待値/終了値:** idle抜去では保持中USBの切断がdaemon状態/診断に現れ、実際に終了したprocessの値をそのまま記録する。stream抜去では切断診断とclient exit 7を確認する。いずれもtime満了exit 0は切断合格でない。新daemonの短TSはexit 0とTS/counter合格。
- **停止/cleanup:** idle/streamそれぞれでowned client/daemonを通常停止してwaitし、再接続後の新daemonも通常停止する。Androidはpermission/FD再取得。
- **記録:** idle/stream、daemon USB owner状態、依頼/応答時刻、path、status/診断、各実終了値、再取得TS/counter、cleanup。

### T08 通常stop/cleanup

- **適用対象/頻度:** 全test終了時。
- **準備:** 各owned client/daemon PIDとruntimeを記録してから起動する。
- **実操作/利用者への依頼:** clientを通常終了しwait "$P"、daemonは通常SIGINT/console stopしwait。
- **期待値/終了値:** 全owned process終了、TS close、FD/IPC/socket/runtime残留なし。timeoutはfail。
- **停止/cleanup:** 無関係processへsignalしない。test専用runtimeだけ片付け失敗log保持。
- **記録:** PID/role/exit、process/FD/IPC/socket/runtime、復旧状態。

### T09 TS/counter/exit判定

- **適用対象/頻度:** 全capture/soak。
- **準備:** TS parser/SPEC条件とdaemon counterを確認できるようにする。
- **実操作/利用者への依頼:** ASICEN SPECの188-byte/sync/TEI/continuity基準とdaemon countersを各TS/client別に確認。
- **期待値/終了値:** 全TSが製品SPEC基準を満たし通常取得exit 0。非空file/counter 0だけでpassにしない。
- **停止/cleanup:** 元TS/hash/logを残してT08 cleanup。
- **記録:** parser/version、TS hash、sync/TEI/continuity/drop/counter、exit。

### T10 native PC/SC/WinSCard

- **適用対象/頻度:** 対応OS/APIのみ。非対応製品は理由付きN/A。
- **準備:** native OS adapter/consumerとcandidate IFD/DLL pathを用意。
- **実操作/利用者への依頼:** Linux Appendix Aのisolated pcsc-liteでpcsc_scan -r/-cと既存bcas-safe-probe。macOS既存isolated stack。Windowsは既存native transaction consumerがある場合のみ。
- **期待値/終了値:** CLI CARD/CI smokeをnative transaction合格にしない。実consumer/adapter/path/exitが必要。
- **停止/cleanup:** consumer/isolated pcscd通常停止。system PCSC serviceを変更しない。
- **記録:** OS/ABI、module/DLL absolute path、consumer、reader/card、operation/exit。

### T11 LNB/非対応拒否

- **適用対象/頻度:** 必要時。給電対応profileだけ実測。
- **準備:** 対応profile、meter、独立給電の状態を確認。
- **実操作/利用者への依頼:** 対応機は`asicend --device usb:<BUS-PORT> --instance rc --runtime-dir "$RT" --allow-lnb-power`でdaemonを起動し、`asicenctl --device usb:<BUS-PORT> --runtime-dir "$RT" --instance rc lnb-on` / `lnb-off`を各実行してmeter測定する。将来、LNB制御非対応profileを持つ実機が利用可能になった場合だけ、sourceで非対応と確認した機器を選び、誤選択時に出力がないことをmeterで確認してから、同じdaemon commandと公開`asicenctl` commandを使う。`--allow-lnb-power`付きで`lnb-on`を一度呼び、未許可フラグによる一般拒否とprofile拒否を区別する。非対応profileを一意に確認できない候補では実行しない。
- **期待値/終了値:** 対応機はmeter実測を記録する。非対応profileの公開APIは`UNSUPPORTED`・exit 3を返し、GPIO/物理給電は切り替わらずmeter値も変化しないことを確認する。API拒否だけでは物理給電不変の確認にならない。
- **停止/cleanup:** daemonを通常停止し、非対応profileでは給電要求を追加せず0Vと通常配線復帰を確認する。
- **記録:** model/profile、command/response/exit、meter値と条件、cleanup。現所持ASICEN機器は全てLNB対応のため非対応profile拒否の実機確認は未実施・現在実行不可と記録する。対応機試験やmockを代替合格にしない。

### T12 複数device/daemon/access

- **適用対象/頻度:** 必要時、複数device/selector/access変更時。
- **準備:** USB port/selectorとinstance/runtimeをdeviceごとに割り当てる。
- **実操作/利用者への依頼:** asicend --list-jsonで複数USB pathを列挙しport/instance/runtimeを分ける。4 receiver同時はT03。driver unbindしない。
- **期待値/終了値:** device/selector対応一意。別deviceの結果を混同せず、要求時は各sideを独立判定。
- **停止/cleanup:** 各client/daemon個別wait/stopしFD/IPC/socket確認。
- **記録:** device/port/PID/runtime、各exit/継続/再取得/access/cleanup。

### T13 soak/頻度

- **適用対象/頻度:** 毎releaseおよび全model初回/major。
- **準備:** 確定頻度表の対象archive/model/timeをmatrixへ展開する。
- **実操作/利用者への依頼:** 毎release代表Linux x86_64 glibc 2h/全archive native short; 初回全modelは代表機all-free-channel 10s、macOS/Windows 30m、他ABI 10m。
- **期待値/終了値:** 指定連続時間完了かつTS条件合格。追加soakは明示ユーザー決定分のみ。
- **停止/cleanup:** T08通常stopと全resource cleanup。
- **記録:** model/OS/ABI/archive、時間、TS/counter、FD/RSS、未実施理由。

### T14 failure/release disposition

- **適用対象/頻度:** 全candidate gate完了後。
- **準備:** source/archive SHAと全T-IDの結果を版別記録へ用意。
- **実操作/利用者への依頼:** version recordで全T-IDをpass/fail/未実施/対象外分類。未完了を公開passにしない。
- **期待値/終了値:** failure/unsupported/未実施/環境不能/candidate欠落をpassにしない。
- **停止/cleanup:** candidateと失敗証拠を保持。tag/releaseは別依頼。
- **記録:** T-ID、candidate/host/model、state、log、blocker、判定根拠。

N/Aは製品機能上該当しない項目だけに使う。OS/ABI候補のunsupported、candidate欠落、未実施、失敗、環境不能は版別結果で未達とし、passに読み替えない。手順に記した期待値は実測結果ではない。

## 0. 共通artifact matrixと共通手順

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

## 2. device-free gate

候補commitの全CI build/test/package/audit job成功を確認する。README記載のnative・cross build、unit test、sanitizer、package展開試験、source archive再buildを行い、出力と終了値を保存する。Linux/macOSで `asicend --version`、`asicen-ts --version`、`asicenctl --version`、`asicend --list-json` を実行し、版情報と妥当なinventory JSONを確認する。USBなしinventoryはscan incomplete等の状態を区別して示す。`--list-json` はdevice ownership不要。異常引数・未知optionのexitとstderrをREADMEのCLI契約に照合する。

## 3. Release archive procedure

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

#### Appendix A. Linux専用PC/SCと既存safe probeの準備

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

Create `$WORK/bcas_safe_probe.c` with the following existing diagnostic source, then build it against the pinned `libaribb25` and this isolated pcsc-lite client library. This probe only reports operation success labels; it does not print ATR, card ID, status fields, or APDU response bytes.

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

macOSでもnative `.bundle`を使うが、system PC/SC serviceをreload/restartしてcandidate IFDを加える操作はしない。`pcsc-tools`由来のnative `pcsc_scan`、safe probe、既存isolated native stackがすべて用意された専用test hostに限って実行する。いずれかがなければ未実施として記録し、host/system PCSCへ接続せず、Linux結果も代用しない。AndroidにはIFDがない。

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
