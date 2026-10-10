# Issue #4: PX4 共通処理への整合

## 基準と範囲

- 実装の起点: ASICEN `42c92a6b27247498f1c2920fd98bb20a346e59e4`（`release/0.1.0`）。そのコミットの Termux/runtime 修正を保持。
- 比較先: PX4 `1a1485d0c3e972e0a47be907edb67949564aa9a7`。
- 元の全件監査: [Issue #4](https://github.com/Khronos31/asicen-userland/issues/4)。以下の ID は元監査の 127 指摘群に対応する。重複観点を含むため、127 個の独立バグという意味ではない。
- 原則: ドキュメント以外の全ファイルを対象に、汎用の API・処理順・制御構文・path・環境変数・テスト・workflow・配布を揃える。実装の歴史やテストが旧挙動を固定していることは、ハードウェア例外にはしない。
- 実機・USB・カード・LNB・ベンダーコードの実行は今回行っていない。オフライン結果を実機認定へ読み替えない。

## 利用者が明示した例外

1. ASICEN は USB serial を持たない。観測した topology、機種、receiver/source/RF 配線、ASICEN 固有の転送・ファームウェア・リンク変換は保持する。
2. 0.1.0 は Linux・macOS・Android の 8 バイナリ＋対応ソース＋SHA256SUMS。Windows のコード・ビルド・テスト・パッケージ生成は残し、配布集約への追加箇所だけをコメントアウトする。WinSCard が揃った 0.2.x で正式対応する予定。VERSION は変更しない。
3. firmware/asicen-loader.bin は保持・再配布する。PLEX からの継続入手が不確実なことへの備えであり、PX4 の非同梱方針には揃えない。公式モジュールに GPL メタデータはあるが、ファームウェアへの適用範囲・全文の根拠・再配布許諾は確定していない。既存のバイナリ同梱／対応ソース除外を維持する。
4. 過去の測定値・usbmon・機種別検証結果を PX4 の結果で置き換えない。試験の履歴と新候補の認定を分ける。

## 主な変更

- 共通 IPC、card、worker、lease、Windows platform、TS client を固定 PX4 へ更新。必要差は `third_party/px4-userland/UPSTREAM.md` に記録。
- 列挙・選択・再列挙・runtime lock・引数・エラー・signal・出力・クリーンアップを共通契約へ整合。
- 旧研究用 24-byte IPC の実行経路を廃止し、mock も canonical runtime/instance サービスへ統一。旧 `--socket` は usage 2。
- TS 受信の境界・順序・品質統計・drain・所有権、LNB 15→0 の適用時点、fallible thread launch と rollback を修正。
- ビルド option/schema、IFD、Termux、source/relink、配布 manifest/audit、platform CI を共通化。古い配布 entrypoint は canonical 実装への薄い wrapper とする。

## 指摘群ごとの実装・検証

各項目の「実装済み」は静的差分の修正を表す。最終 CI または実機の未検証を同時に記載し、未確認の条件まで合格としない。

| ID | 元監査の指摘 | 対応と残条件 |
| --- | --- | --- |
| AN001 | 実ハードウェアにも backend=mock-only を表示する | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN002 | 列挙 JSON の receiver/device・USB位置・LNB能力が不一致 | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN003 | --list と単機能機種の列挙構造が欠ける | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN004 | 全筐体・全instance・mockが固定 /tmp lock で競合・起動拒否になる | 共有 identity lease と canonical runtime root に統一。serial の代用は観測 topology のみ。inode/owner/mode/link と跨 instance 競合の回帰を維持。 |
| AN005 | 既存lock inodeの検証と競合リトライが弱い | 共有 identity lease と canonical runtime root に統一。serial の代用は観測 topology のみ。inode/owner/mode/link と跨 instance 競合の回帰を維持。 |
| AN006 | USB選択値の十進検証とbase0再解析が食い違う | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN007 | daemon argument契約・help・診断が分岐実装でずれる | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN008 | LNB 15→0 の適用時点がPX4と逆 | LNB の ON/OFF とも tune 前に適用し、失敗時 rollback。15→0 回帰を更新。電気的測定は未実施。 |
| AN009 | reenumeration後の明示選択が別筐体へ広がる | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN010 | USB completion lengthの防御検証がない | completion 長の検証・投稿順配送・partial error 非公開・有限 drain/所有権隔離を整合。fault injection で検証。 |
| AN011 | completion配送順とpartial error扱いを揃える | completion 長の検証・投稿順配送・partial error 非公開・有限 drain/所有権隔離を整合。fault injection で検証。 |
| AN012 | stream readの引数検証とdetach後drain契約が異なる | 188-byte 単位、最大長、detach 後 tail/final identity を共通契約へ変更。境界と最終 drain の回帰を追加。 |
| AN013 | TS countersの意味と品質検出が不足する | TS 品質・受理済 input・実出力・discard の意味を揃えて計数。0 の値だけで無誤りとは判定しない。 |
| AN014 | framer/queueの容量・再試行・allocation方針が別実装 | Result/ByteView/sink と bounded carry、packet-count ring/min/max、retry に統一。attach 前配送を防止。 |
| AN015 | callback drain期限と致命処理が三系統ある | completion 長の検証・投稿順配送・partial error 非公開・有限 drain/所有権隔離を整合。fault injection で検証。 |
| AN016 | USB/timeout/disconnect/firmwareエラーを潰す | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN017 | platform共通のlibusb context lifecycleが欠落 | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN018 | signal失敗・loader待機cancel・出力失敗を落とす | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN019 | 現在の1/2 receiver・single lease制限は未実装として記録する | 一部未確定: 全機種・USB機能・RF/source の経路と汎用 ownership を検証。shared seed/controller05/最後のpeer停止のハードウェア規則が未確定のため同時受信能力は拡張しない。 |
| AN020 | --socket token全走査で別protocolへ誤dispatchする | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN021 | research IPC client/serverの汎用I/O・認証・leaseが別仕様 | 旧24-byte研究IPCの実行経路と旧model APIを除去。mock も製品 protocol/lifecycleへ統一。partial frame/blocked output/packet 回帰を移行。 |
| AN022 | SHA-256/entropy/file-readを重複実装している | FirmwareProvider/Image、正確な共有 SHA、共有 OS nonce provider に統一。旧 file-read/RNG 実装を除去し、境界・エラー・partial seed 非公開を検証。 |
| AN023 | 低層builderの公開境界をhardware例外にしない | I2C/CF/DSC builder の長さ・lane・staging上限を検証し invalid marker は transport 前に拒否。wire 数値は保持。 |
| AN024 | 診断CLIのparser/output/cleanup契約が製品CLIと別 | frontend/TS/card の直接 probe と個別 pinned parser、acceptance、出力/期限/cleanup を実装。raw ASICEN 診断の値は hardware 境界として保持。 |
| AN025 | offline transformのEOF/出力契約とprobe表示が独立 | frontend/TS/card の直接 probe と個別 pinned parser、acceptance、出力/期限/cleanup を実装。raw ASICEN 診断の値は hardware 境界として保持。 |
| AN026 | テスト選択と期待値が製品parityを証明しない | 製品 profile の codec/worker/boundary と実装へ接続した回帰を追加。旧仕様の oracle は要求された契約へ移行。テスト成功だけで全差分消滅とは扱わない。 |
| AN027 | 一時path/fixture/harnessも揃えられる | runtime/fixture の一時パスを共有規則へ移行。標準 temp root と短い Termux root、独立 fixture cleanup を使用。 |
| AN028 | 構文・for/if・error表示・header構成の整合が残る | 汎用 for/if、個別 option 分岐、include guard、Result/sink、return-based fixture を整合。機械 formatting と意味差検証は別に実施。直接の参照先と一致する Windows policy の accumulator は保持。 |
| AN029 | backend interfacesを機種非依存roleへ再接続する | 製品は canonical tuner/card/stream/firmware APIへ接続。重複 research model を除去、decoder は Result/ByteView/sink。CardOperationGuard、EnclosureOwnership、StreamCaptureSource、raw CaptureIo/QueueWait は ASICEN の複数USB機能・共有制御・生転送を結ぶ adapter として残る。これらの排他・期限・first error・drain/close 順も検証対象で、wire-only として一括免除しない。 |
| AN030 | portable TS wrapperはpublic名とentry分割だけの差として分離 | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| AN031 | 明示TSID=0までempty sentinelとして拒否する | 明示 TSID=0 と空slot自動探索を分離。期限最終境界、first error、rollback と対応する hardware encoding を検証。 |
| N01 | native TS readの引数・error・上限を共通契約へ揃える | 188-byte 単位、最大長、detach 後 tail/final identity を共通契約へ変更。境界と最終 drain の回帰を追加。 |
| N02 | detach後のqueued tailとfinal identityの寿命が不一致 | 188-byte 単位、最大長、detach 後 tail/final identity を共通契約へ変更。境界と最終 drain の回帰を追加。 |
| N03 | TS統計が同じfield名でも違う事象を数える | TS 品質・受理済 input・実出力・discard の意味を揃えて計数。0 の値だけで無誤りとは判定しない。 |
| N04 | queue/framerは同じbounded stream処理へ寄せられる | Result/ByteView/sink と bounded carry、packet-count ring/min/max、retry に統一。attach 前配送を防止。 |
| N05 | 初期TS安定化閾値と同期証明長はhardware値と汎用処理を分離する | 汎用同期証明を完全な4 packet に整合。ASICEN 固有の開始・安定化条件は根拠のない IT930x 値で置換しない。 |
| N06 | firmwareのpath・I/O error・hash実装をhardware imageから分離して揃える | FirmwareProvider/Image、正確な共有 SHA、共有 OS nonce provider に統一。旧 file-read/RNG 実装を除去し、境界・エラー・partial seed 非公開を検証。 |
| N07 | native daemon引数parserのportable contractと分離構造を揃える | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| N08 | LoggerとMockTransportはportable source/API/test欠落として扱う | 固定 PX4 の Logger/MockTransport API・実装と同等testを導入。ASICEN raw capture は endpoint0x81/0x82 の実 wire adapter で試験。 |
| N09 | libusb API seamsとend-to-end lifetime oracleを揃える | LibusbAcquisitionApi/LibusbContext と失敗時 ownership を実装。descriptor/config/open/claim/altsetting/FD/quarantine の注入試験で release→close→context 順を検証。native試験とWindows cross-build、独立再検証を通過。 |
| N10 | developer probe全flag・処理crosswalkを確認しportable差を特定 | frontend/TS/card の直接 probe と個別 pinned parser、acceptance、出力/期限/cleanup を実装。raw ASICEN 診断の値は hardware 境界として保持。 |
| N11 | 同じ非hardware処理のfor/if・RAII・関数分割を揃える | 汎用 for/if、個別 option 分岐、include guard、Result/sink、return-based fixture を整合。機械 formatting と意味差検証は別に実施。直接の参照先と一致する Windows policy の accumulator は保持。 |
| N12 | 必要なhardware差はpayload・mapping・electrical順序へ限定する | 機種ごとの register/payload/RF/source/USB endpoint/electrical 値と製品名を保持。ファイル全体を hardware 例外にはしない。 |
| N13 | native testの移植は不存在判定と既存同等coverageを区別する | 製品 profile の codec/worker/boundary と実装へ接続した回帰を追加。旧仕様の oracle は要求された契約へ移行。テスト成功だけで全差分消滅とは扱わない。 |
| N14 | Android最終linkのretained dependency検証を揃える | Android 最終link map/sentinel・依存closure を shared builder と検査へ接続。実 NDK/ABI CI は別途必要。 |
| W01 | Windows IPC実装と製品3コマンドのbuild経路が未移植 | Windows IPC/nonce/sleep/UTF-8 argv/output/signal/stdin と製品3コマンドを移植。cross-build 済。0.1.0配布除外は明示user例外。 |
| W02 | Windows CSPRNG nonceとhigh-resolution sleepをOS共通契約へ追加する | Windows BCrypt nonce と共有 high-resolution sleep に統一。重複 native RNG を廃止。 |
| W03 | Windows TS binary output・Unicode argv/path・最終drainを共有runnerへ接続する | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| W04 | daemonのcooperative stopとPOSIX signal failure handlingを揃える | 製品入口・列挙・parser・error・platform lifecycle を共通化。tools/parser/firmware preflight 回帰を追加。プロセス間実行は CI で確認。 |
| W05 | Windows専用offline testとshared worker testのWindows入口が欠落 | Windows offline suite と共有 worker/argument/platform tests を登録。native Windows CI は最終commitで確認。 |
| W06 | WinUSB RAW_IO policyのportable算術・error handling・testsを分離して再利用する | RAW_IO を dedicated Windows source に分離。packet算術、上限、失敗/復元の offline tests を共通化。 |
| W07 | portable部分の関数/for/if/名前/include構成をhardware差から切り離して統一する | 汎用 for/if、個別 option 分岐、include guard、Result/sink、return-based fixture を整合。機械 formatting と意味差検証は別に実施。直接の参照先と一致する Windows policy の accumulator は保持。 |
| W08 | 必要差はserial identity・hardware fixture・product名称に限定する | 機種ごとの register/payload/RF/source/USB endpoint/electrical 値と製品名を保持。ファイル全体を hardware 例外にはしない。 |
| W09 | 参照Windows実装にもdeadline/一時path/cleanupの留保があるため無条件コピーしない | 参照実装の既知留保を無根拠に修正済みとはしない。追加の bounded output/fallible thread/quarantine は明示し regression を付ける。 |
| HT01 | 15V→0V retuneのOFF適用時点がPX4と逆で、既存testが差を固定している | LNB の ON/OFF とも tune 前に適用し、失敗時 rollback。15→0 回帰を更新。電気的測定は未実施。 |
| HT02 | disconnect後のmetadata-only・冪等性・生存側cleanupを単一契約で照合する | libusb acquisition/lifetime seam、切断・取得失敗 rollback、冪等close と quarantine を試験。release→close→context と生存側cleanupの順を確認。実機抜線試験は未実施。 |
| HT03 | multi-owner/permutation/concurrency testを機種数の違いだけで免除しない | 汎用部分は両laneのclose/reopen順、重複・非owner release、32回の同時取得競合で単一ownerを検証。全機種のRF/source経路も検証。複数同時ハードウェア受信はAN019の未確定条件として別記。 |
| HT04 | failure precedence・rollback・cleanup debtのportable契約を既存強力なASICEN testsへ対応付ける | 最初のoperational errorをcleanup/rollbackで上書きしない。tune/open/LNB/card/stream の失敗注入と後始末を対応付ける。 |
| HT05 | mock・for/if・assertion・runnerのportable表記を揃え、exitによるdestructor省略を分離する | 汎用 for/if、個別 option 分岐、include guard、Result/sink、return-based fixture を整合。機械 formatting と意味差検証は別に実施。直接の参照先と一致する Windows policy の accumulator は保持。 |
| HT06 | 例外はregister・wire bytes・実配線・電気順序に限定しtest全体を免除しない | 機種ごとの register/payload/RF/source/USB endpoint/electrical 値と製品名を保持。ファイル全体を hardware 例外にはしない。 |
| HT07 | direct TSID=0拒否とdeadline最終境界の差をhardware根拠付きで解決する | 明示 TSID=0 と空slot自動探索を分離。期限最終境界、first error、rollback と対応する hardware encoding を検証。 |
| I01 | ビルド基盤の VERSION 読込みと CMake option schema | VERSION厳密読込み、option/default、warnings/no-exception/RTTI、libusb検出、IFD/exportを共通化。 |
| I02 | 共通 compile policy と target 定義の書き方 | VERSION厳密読込み、option/default、warnings/no-exception/RTTI、libusb検出、IFD/exportを共通化。 |
| I03 | libusb discovery、最小版、override の引数表現 | VERSION厳密読込み、option/default、warnings/no-exception/RTTI、libusb検出、IFD/exportを共通化。 |
| I04 | IFD build の矛盾 option と dependency / symbol 境界 | VERSION厳密読込み、option/default、warnings/no-exception/RTTI、libusb検出、IFD/exportを共通化。 |
| I05 | IFD artifact / template の名称・コメント・配置 | VERSION厳密読込み、option/default、warnings/no-exception/RTTI、libusb検出、IFD/exportを共通化。 |
| I06 | Android build CLI・ABI 既定値・modified libusb 入力 | Android ABI/NDK/input/temp/env/最終link検査を整合。 |
| I07 | Android toolchain validation と出力集合 | Android ABI/NDK/input/temp/env/最終link検査を整合。 |
| I08 | Android tempdir・publish 原子化 cleanup・環境制御 | Android ABI/NDK/input/temp/env/最終link検査を整合。 |
| I09 | Android ELF verifier の唯一の小差 | Android ABI/NDK/input/temp/env/最終link検査を整合。 |
| I10 | Linux static builder の interface・toolchain・出力責務 | Linux/IFD/macOS builder・再現性・Libs.private・staging stripを整合。強い既存musl証拠は保持。 |
| I11 | Linux IFD builder の compiler / license / test policy | Linux/IFD/macOS builder・再現性・Libs.private・staging stripを整合。強い既存musl証拠は保持。 |
| I12 | macOS build の TMPDIR / deployment / path reproducibility | Linux/IFD/macOS builder・再現性・Libs.private・staging stripを整合。強い既存musl証拠は保持。 |
| I13 | macOS Libs.private forwarding と strip の段階 | Linux/IFD/macOS builder・再現性・Libs.private・staging stripを整合。強い既存musl証拠は保持。 |
| I14 | Termux supervisor・signal・有限cleanupが欠ける | Termux supervisor/argv/signal/有限cleanupと完全な回帰matrixを移植。 |
| I15 | Termux option / FD重複 / argv組立て | Termux supervisor/argv/signal/有限cleanupと完全な回帰matrixを移植。 |
| I16 | Termux regression suite の内容・実行接続 | Termux supervisor/argv/signal/有限cleanupと完全な回帰matrixを移植。 |
| I17 | Android archive 展開後の launcher と daemon の相対位置 | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I18 | archive directory layout は名前置換以上に異なる | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I19 | metadata / source manifest / checksum schema | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I20 | source snapshot の exact commit / tree 保証 | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I21 | Linux binary audit の strip / RPATH / build-id / exports | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I22 | macOS binary audit と host実行証拠 | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I23 | Android static link inventory と NDK provenance | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I24 | archive allowlist・duplicate・link/path safety | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I25 | package scripts の構成・CLI・ラッパー名 | canonical package/source/audit、flat layout、exact snapshot、manifest/hash、安全なmemberを統一。旧entryは薄いwrapper。 |
| I26 | modified libusb relink proof のtargetと証拠 | 対応ソースからのLinux/macOS/Windows再リンクとAndroid modified-libusbをCIへ接続。最終commit snapshotの監査を別途記録。 |
| I27 | CI filenames / triggers / action pinning / permissions | workflow/triggers/pins/permissions/timeouts/matrixとexact artifact/source照合を整合。 |
| I28 | CI image / compiler / apt policy と offline test matrix | workflow/triggers/pins/permissions/timeouts/matrixとexact artifact/source照合を整合。 |
| I29 | final release-candidate の再監査と archive smoke | workflow/triggers/pins/permissions/timeouts/matrixとexact artifact/source照合を整合。 |
| I30 | packaging / workflow の negative regression harness | workflow/triggers/pins/permissions/timeouts/matrixとexact artifact/source照合を整合。 |
| I31 | Windows x64 build / packaging / native tests の欠落 | Windows実装/build/package/native testは保持。0.1.0の配布集約への追加部分のみコメントアウト（WinSCard/0.2.xで復帰）。 |
| I32 | Fedora service / SELinux / Polkit / sysusers の未対応 | Fedora/mdev/service/libusb互換checkを機種ID/topologyに適用。ホストへinstallは未実施。 |
| I33 | BusyBox mdev helper / startup / permission regressions | Fedora/mdev/service/libusb互換checkを機種ID/topologyに適用。ホストへinstallは未実施。 |
| I34 | libusb compatibility maintenance / reusable checks | Fedora/mdev/service/libusb互換checkを機種ID/topologyに適用。ホストへinstallは未実施。 |
| I35 | repository text policy / ignore / agent guidance filename | editor/LF/ignore共通化、ユーザーのruntime/Termux変更保持。AGENTS/CLAUDEにparity原則。 |
| I36 | historical oracle 4本のhardware限定例外とgeneric処理を分離 | 歴史的oracleの汎用bounds/signal/deadline/I/Oを修正。vendor ABI/実測値を保持。SDK header欠如のharness compileは未確認。 |
| I37 | TS validator はhardware-traces下でも汎用実装 | TS validator実装をuserland/toolsへ移し履歴pathはwrapper。schema/countersを保持、self-test通過。 |
| I38 | vendor firmware・fetch/extract は必要なbytesと配布policyを分離 | firmware保持/同梱と別ライセンスの根拠を保持。canonical notice/license配置、依存source/hashを検証。 |
| I39 | license / provenance のファイル名と事実の切分け | firmware保持/同梱と別ライセンスの根拠を保持。canonical notice/license配置、依存source/hashを検証。 |
| I40 | dependency licenseの取得/配置を統一する | firmware保持/同梱と別ライセンスの根拠を保持。canonical notice/license配置、依存source/hashを検証。 |
| I41 | README installation / tempdir / PCSC guide の差 | READMEのruntime/readiness/trap、Termux短path/setsid、PC/SC四項目とgroup/bundle説明を現実装へ整合。 |
| I42 | SPEC の baseline が古く現行配布契約と不一致 | 正本baseline/Windows配布例外/firmware保持の現在方針を明記し旧scopeとの矛盾を解消。 |
| I43 | release手順の変更影響・再現性・tag artifact照合 | exact commitの独立2run、candidate/tag/Draft/public payload検証を文書化。今回公開・実機検証を実行した主張ではない。 |
| I44 | ASICEN soak手順の内部矛盾 | 既決定の環境別soak（2h/30min/10min）を保持し一律2h/16h記述を訂正。新たな物理試験を実施しない。 |
| I45 | PX4側の古いnoticeもそのままコピーしない | 参照の古いnotice/実測policyを新規認定へコピーしない。現行規範と明示user例外を適用。 |
| I46 | 履歴・実測trace・異機種の認定は非対称のまま保持 | 履歴測定値とraw traceを保持。旧合格を変更後candidateの合格へ転用しない。 |
| I47 | platform how-to と過去動作確認を分離 | platformの実測PASSは移植しない。導入用service/configと現行READMEを整合。純文書の全文同一化は元依頼の対象外。 |
| I48 | import manifest と実稼働fileの対応 | UPSTREAMを固定pin、実際のbuild対象、完全なimport一覧、必要delta/testへ更新。 |
| I49 | W3U4 report utility のhardware差と共通report機能 | ASICEN reportを同じgeneric report/cleanupで実装。topology/modelのみ機器差、mock9ケースを検証。 |
| I50 | micro-style / temporary path / environment / for-if 全件の追跡 | 汎用 for/if、個別 option 分岐、include guard、Result/sink、return-based fixture を整合。機械 formatting と意味差検証は別に実施。直接の参照先と一致する Windows policy の accumulator は保持。 |
| I51 | macOS version smoke の独立ファイルと検査責務 | 独立version-smoke重複を共有relinkへ。IFD proxy roundtripはcanonical auditorに移管。socket実行はCI待ち。 |
| I52 | hardware oracle / report内のgeneric処理を包括免除しない | 歴史的oracleの汎用bounds/signal/deadline/I/Oを修正。vendor ABI/実測値を保持。SDK header欠如のharness compileは未確認。 |
| I53 | Git executable bit と末尾LFの差 | script executable bitと末尾LFを検査・整合。 |
| PC-01 | ASICENの停止処理をPX4基準へ揃え、不可欠な取消だけ根拠を残す | listener/accepted work/drain/joinは固定版。ASICEN cancel通知だけ最小 adapterとして保持し後始末を抑止しない。 |
| PC-02 | ASICENのcard初期化をPX4へ揃え、取得済ATR入口の必要性を確認する | live CardSession初期化は固定版と一致。取得済ATR入口はprobeで二重resetを避ける限定adapterとして検証。 |
| PC-03 | PX4 Windows対応をportable coreの全呼出境界へ揃える | Windows IPC/nonce/sleep/UTF-8 argv/output/signal/stdin と製品3コマンドを移植。cross-build 済。0.1.0配布除外は明示user例外。 |
| PC-04 | ASICENのtopology必要差だけを残してPX4の処理形へ揃える | receiver/source table/count/magic の機器・製品値だけ保持。codec/worker境界と共有処理を整合。 |
| PC-05 | serialなしの機器事実と汎用server入力契約を切り分ける | 共有 identity lease と canonical runtime root に統一。serial の代用は観測 topology のみ。inode/owner/mode/link と跨 instance 競合の回帰を維持。 |
| PC-06 | PC/SCの必要な製品値だけを残してASICENをPX4へ揃える | PC/SC capability/name/max19200と共有IPCを整合。IFDはheaders-only、実USB・pcsc client依存なし。 |
| PC-07 | runtime/lock名の設定点と小さな記法差まで揃える | 共有 identity lease と canonical runtime root に統一。serial の代用は観測 topology のみ。inode/owner/mode/link と跨 instance 競合の回帰を維持。 |
| CLI-T01 | asicenctlの固定mock-only表示と--group help欠落を解消する | active/vendored ctlのmock-only表示を除去しgroup helpと共有formatterに統一。 |
| CLI-T02 | Windows引数/worker poll試験の可搬層を共通化する | Windows argv/pollとTS parser/runnerを固定版へ。POSIX entryの重複分割を廃止。 |
| CLI-T03 | TS parserとusageを必要な製品差だけへ縮小する | Windows argv/pollとTS parser/runnerを固定版へ。POSIX entryの重複分割を廃止。 |
| CLI-T04 | product-wire codec試験を移植し選抜runnerをnamed registryに揃える | 完全なcodec golden suiteとnamed registryを製品profileでも登録。両profileの固有magic/countだけ区別。 |
| CLI-T05 | USB policy/acquisition回帰契約を実際のASICEN transportへ対応付ける | 実際のLibusbDevice/LibusbFunctionClaim/EnclosureOwnershipへ注入し取得・claim・cleanup順を検証。RAW_IO境界、Windows ACCESS→BUSYも回帰対象。native試験とWindows cross-buildを通過。 |
| RD-01 | Customer_Info traceのfirmware省略説明と保存内容を一致させる | 保存traceの20loader submitにtruncated payloadが残る事実へ説明を訂正。既存setup/status/length/order/bytesは変更しない。 |

## 検証結果

- Linux の libusb ON・PC/SC IFD ON・診断ツール ON・全テストのビルド成功。ローカル CTest は 71 件中 61 件成功、10 件が下記の実行環境制限で失敗。合格に読み替えたり skip を追加したりしていない。
- 制限対象: `px4-portable-tests`、`px4-pcsc-ifd-tests`、`asicen-card-only-service-tests`、`asicen-libusb-fd-tests`、`asicen-hardware-ipc-integration-tests`、`asicen-product-shutdown-tests`、`asicen-product-worker-routing-tests`、`asicen-product-cli-integration`、`asicen-mock-integration`、`asicen-ifd-bundle-smoke`。この実行環境では AF_UNIX socket 作成が EPERM、native libusb context 初期化も失敗する。未変更の起点にも同じ種類の失敗があり、別途許可された実行でも制約は同じだった。
- libusb/IFD/tests/tools OFF の最小製品ビルド成功。Windows は固定 LLVM-MinGW と static libusb で製品 3 コマンドおよび offline test 実行ファイル群を cross-build。Windows 上での実行合格とは区別する。
- packaging suite、Termux signal/cleanup、workflow、Fedora/mdev、report の模擬試験を通過。static/source 25＋macOS 5＋IFD allowlist 8 の配布回帰を通過。Windows の CI 用 ZIP は real PE の import/static-libusb/firmware/license/checksum と deterministic 再生成を監査した。
- Logger/MockTransport、nonce、firmware、codec、IFD capability、TS parser、libusb acquisition/lifetime、stream/framer、diagnostic deadline/final event、fixture failure cleanup を個別に再検証。実機受信を伴わない。
- SDK に依存しない歴史的 oracle 3 本の構文検査と validator self-test を通過。`official-trace-harness.cpp` は vendor `Data_define.h` がこの環境にないため全体compile未確認。ベンダーコードは実行していない。
- Git の正確な commit からの対応ソース export と各 OS/ABI の CI 結果は PR で報告する。未commitの作業ツリーを既存 HEAD の成果物として偽装しない。過去の実機PASSを変更後バイナリへ転用しない。

## 初回 CI で確認したことと修正

`2c2f769` の PR CI では Ubuntu の全 71 CTest、Windows Server 2022 の offline implementation tests、Android 3 ABI、各対応ソース relink が成功した。配布・他 OS の後段で次の不備が見つかったため、後続コミットで修正する。初回 CI 全体は失敗であり、後続コミットの合否は PR に記録する。

- `*.d` ignore に該当する新規 reader/sysusers テンプレートが追跡から漏れていた。両ファイルを追跡し、Git commit の source export に存在することを回帰で確認する。
- macOS IFD symbols test に bundle path が渡っていなかった。固定 PX4 と同じ引数を復元する。
- Alpine の全件試験に必要な bash と、bind mount した `/src` の Git ownership 前提が欠けていた。使い捨て CI コンテナ内の必要箇所を修正する。
- FD safety test が不要なデバイス列挙を伴う context を初期化していた。実製品の FD 経路と同じ no-discovery 初期化へ揃え、native libusb の pipe 拒否と caller FD 保持の assertion は維持する。

## 残るハードウェア条件

AN019 の shared seed/controller05 と最後の peer 停止時の規則は、複数ストリームの安全な同時開始・停止を確定する根拠が不足している。物理的に不可能とは判断せず、未実装・未検証として現状の能力上限を明示する。汎用の経路・ownership テストを省略する理由にはしない。

N05 の 4 完全 packet 同期は汎用フレーミングの整合である。ASICEN 側の開始直後の stale data や安定化バッファ条件が解明されたという意味ではなく、IT930x の固有 drop/閾値を根拠なくそのまま移さない。次の実機確認や公式トレース照合では、この開始条件と AN019 の peer 開始・停止・最後のowner解放を分けて観測する。
