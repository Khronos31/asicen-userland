# リリース記録と検証 matrix

PX4 と ASICEN で、工程、証拠の語彙、matrix の記録構造、公開前の機械照合を共通にします。機種・配布対象・物理試験・受入条件は各製品の正本に従います。共通形式への記入だけでは新しい hardware claim を作りません。

## 記録の役割

- [release-validation.md](release-validation.md): 実機・仕様変更試験の手順と環境
- [実測記録](../HARDWARE-VALIDATION.md): 日付・試行ごとの結果、失敗、根拠、利用者の判断。既存の履歴は変更しない
- `release-notes/vX.Y.Z.md`: 明示的な作成依頼の後に書く利用者向け本文
- `release-notes/vX.Y.Z.json`: 公開を検査する小さな sidecar。実測記録への参照と exact candidate identity を保持

版ごとの Markdown/JSON は候補の source inventory に存在する必要があります。試験前は [無効な placeholder](../release-notes/README.md) だけを置きます。物理測定の収集や試験全体を一つの汎用 script に置き換える仕組みではありません。

## 共通の語彙

| 記録状態 | 意味 |
|---|---|
| 継承 | 同じ model × runtime/access path × feature の既存証拠を、byte identity または変更影響の根拠により使う。今回実測したという意味ではない |
| 今回再検証 | 今回の exact candidate で試験した。合否は別欄へ記録し、失敗試行もこの状態で残す |
| 未認定 | 必要な実測が未実施、未完了、または合格を確定できない |
| 対象外 | 製品仕様・配布方針・当該 feature の非該当を根拠付きで記録。環境不足や失敗の隠蔽には使わない |

結果欄は `未実施` / `合格` / `失敗` / `判定保留` / `非該当` を区別します。`build-tested` は build/offline の事実だけです。`hardware-unverified` との併記を妨げず、tuner/card/native adapter の認定範囲をまとめて拡大しません。必須の artifact 別短時間 matrix は、他の証拠を `継承` しただけでは完了しません。

## 共通の人手記録テンプレート

以下は空の記録用見本であり、現在候補の実測ではありません。既存の実測記録へ必要な行を追記します。表の access path は予定を示すだけで、実施した host/path を実値で残します。E 番号は環境の共通参照であり、同じ番号を異なる archive の結果の代用にはしません。

### 候補・変更影響

- 対象 version / baseline tag / 日時 UTC:
- candidate source SHA / tree / run ID / run attempt / artifact ID:
- reproduction source SHA / tree / run ID / run attempt / artifact ID:
- 各 run の URL、全 archive と外側 `SHA256SUMS` の SHA-256:
- runner label / OS image version と、実効 compiler / SDK / NDK / dependency revision・digest:
- 累積 diff の変更箇所・呼出経路・platform guard と影響する model/path/feature:
- 継承する baseline evidence、失効/非影響の理由、追加の仕様変更試験:
- soak を決めた利用者の日時・指示、対象 OS/archive/model、時間または実施なしの判断:
- 未認定・既知の制限・受入判断の対象版と範囲:

### 配布 binary matrix（8 行）

| target | 環境 ID | OS / native architecture | host / access path | archive SHA-256 | 状態 | 結果 | 証拠 |
|---|---|---|---|---|---|---|---|
| `linux-glibc-x86_64` | E03 | Linux x86_64 / glibc IFD | native libusb | 未記録 | 未認定 | 未実施 | 未記録 |
| `linux-musl-x86_64` | E02 | Linux x86_64 / musl IFD | HAOS Alpine/musl（実際の経路を記録） | 未記録 | 未認定 | 未実施 | 未記録 |
| `linux-glibc-aarch64` | E15 | Linux aarch64 / glibc IFD | native libusb | 未記録 | 未認定 | 未実施 | 未記録 |
| `linux-musl-aarch64` | E15 | Linux aarch64 / musl IFD | native aarch64 / musl / physical USB | 未記録 | 未認定 | 未実施 | 未記録 |
| `darwin-arm64` | E04 | macOS arm64 | native libusb / PC/SC | 未記録 | 未認定 | 未実施 | 未記録 |
| `android-aarch64` | E05 | Android aarch64 | Termux / launcher / FD | 未記録 | 未認定 | 未実施 | 未記録 |
| `android-armv7a` | E06 | Android armv7a | Termux / launcher / FD | 未記録 | 未認定 | 未実施 | 未記録 |
| `android-x86_64` | E07 | Android x86_64 | Termux / launcher / FD | 未記録 | 未認定 | 未実施 | 未記録 |

Windows は 0.1.0 の binary inventory と実機 gate から対象外です。ソース・CI の Windows build/offline 結果は別に記録し、正式対応を示しません。FreeBSD と Android APK も対象外です。既存 loader は binary に同梱、source には非同梱であり、未解決の再配布権を保持します。

### 各行・各試行に残す詳細

- exact archive 名・SHA-256・candidate source SHA/run、展開した実行ファイル、実施日時
- 実際の host、OS/version、native CPU architecture、libc、runtime、libusb/launcher/adapter の版
- model/profile、USB ID、観測した device identity/topology と接続位置
- native/container/add-on、権限、usbip、LSM、Termux FD、PC/SC など実際の access path
- 試験した feature とコマンド、終了値、TS packet/counter、card status/ATR/APDU、復旧と process/FD/socket 残留
- 実施を依頼した物理操作・利用者の実施確認、未完了または非該当の理由
- raw log / 測定 / 失敗試行 / 再試行の保存先と、結果を読めるコミット済み evidence への参照
- soak の開始終了・時間・負荷・FD/RSS 経時傾向・判定と、必要な機種固有試験

ASICEN 0.1.0 で既決定の条件を保持します。8 行の短時間回帰・LNB 測定、代表機の全チャンネル回帰、代表機と macOS の 2 時間・その他 Linux の 30 分・Android 3 ABI の 10 分 soak は release-validation.md に従います。PX4 の新しい通知コマンドや soak 選択を追加しません。以後の LNB 再測定省略条件と各版の soak 判断も同文書に従います。

## 機械可読 sidecar: schema 1

ファイルは `release-notes/vX.Y.Z.json` です。以下のキーだけを持つ JSON object とし、省略・未知キー・重複キー・偽の値で補いません。version は `VERSION` と同じ `X.Y.Z`、tag は `vX.Y.Z` です。

| トップレベルキー | 値 |
|---|---|
| `schema` | 整数 `1` |
| `product` | `asicen-userland` |
| `repository` | `Khronos31/asicen-userland`（実行する正確な repository） |
| `version` | `X.Y.Z` |
| `notes_sha256` | 完成した同版 Markdown の bytes の SHA-256（64桁 hex） |
| `candidate` | 実機試験に使った候補の run object |
| `reproduction` | 同じ source の独立した第2 run object |
| `gates` | 下記の完了証拠 object |

各 run object の全キー:

| キー | 値 |
|---|---|
| `run_id`, `run_attempt`, `artifact_id` | GitHub の実値、それぞれ正の整数 |
| `sha`, `tree` | full 40桁 hex の source commit / tree |
| `workflow` | `.github/workflows/build_userland.yml` |
| `artifact_name` | `release-candidate` |
| `assets_sha256` | 正確な 10 ファイル名 → SHA-256 の object。外側 `SHA256SUMS` 自体の hash も含む |
| `build_inputs` | 実効 build input 名 → 観測した値。空でない文字列同士の空でない object |
| `runner_images` | runner/job 名 → 実際の OS image version/ID。空でない文字列同士の空でない object |

候補と再現性 run は `run_id` が異なり、`sha`・`tree`・`build_inputs`・`assets_sha256` が一致する必要があります。`runner_images` の違いだけでは不合格にしませんが、実効入力の同一性は証拠で確認します。API の run/attempt/artifact identity と実 bytes の再照合に使う情報であり、候補の単なる branch 名は使えません。

`gates` の全キー:

- `hardware`: 配布 target 名 8 個すべてをキーに持つ object。各値は下記の完了証拠
- `specification_tests`: 変更された仕様の必要試験の完了証拠。追加試験なしなら変更影響と非該当の理由
- `soak_decision`: 利用者の判断と、選ばれた試験を完了した証拠。明示的な soak なしは「実施せず」の判断を残す
- `support_review`: README/SPEC の support 表示と、model/platform/feature の証拠を照合した記録
- `prepublication_review`: license/notices/provenance/source/inventory/checksum/ノートを確認した記録

完了証拠の正確な形式は `{"status":"completed","evidence":["<committed-document-path>#<section>"]}` です。実在するコミット済み文書を参照し、tag source archive 内にも存在させます。例: `HARDWARE-VALIDATION.md#...` または `docs/release-validation.md#...`。 ローカルだけのログは実測文書で保存先を説明し、sidecar の証拠参照そのものに外部 URL やローカル絶対パスを使いません。

`completed` は、担当者が正本の受入条件と利用者判断に基づいて記入する完了の表明です。script は構造・identity・hash・参照先文書の存在と非空の bytes・payload を検査します。Markdown の anchor が実際の節を指すことや、証拠の内容の真偽までは検証しません。担当者は参照先の節を開き、今回の候補・日付・必要条件・利用者判断を確認してから完了を記入します。自動検査は実機の接続、測定値、物理操作の実施、利用者の承認を見たことにはなりません。原記録の失敗・保留を消したり、過去版のリスク受入を今版へ自動継承したりしません。証拠不足なら placeholder のまま止めます。

### 候補から結果コミットへ変えられる文書

公開検査は単なる拡張子判定を使いません。`README.md`、`SPEC.md`、`docs/platforms/validation-results.md`、`docs/release-validation.md`、`docs/releasing.md` と同版の `release-notes/vX.Y.Z.md` / `.json`、ASICEN の `HARDWARE-VALIDATION.md` に限定します。source archive 内の生成 `README.md` / `BUILD-RELINK.md`、明示した commit/tree・文書 hash/size と checksum も整合を要求します。新規ファイルの追加、削除、mode変更を「文書だけ」として通しません。証拠の参照はこの文書集合のうち sidecar 自体を除くファイルに限ります。`docs/release-record.md` や workflow/helper の仕様修正は候補固定前に完了させます。

## 公開後の記録

tag source SHA/tree、tag build run/attempt、公開 workflow run、candidate/reproduction run、tag/candidate payload 比較、Draft/公開後の再取得結果、公開時刻、Release URL と Latest を実測文書に残します。途中失敗時は Draft 作成済み・公開済み等の実際の状態と不一致を記録し、既存タグや公開アセットを上書きしません。

