# asicen-userland

`asicen-userland` は、ASICEN チップを搭載した PLEX 製 USB チューナーに対応した、ユーザー空間で動作するドライバおよびツール群です。カーネルモジュールを使用せず、ユーザー空間から選局、MPEG-TS ストリームの取得、および内蔵 IC カードリーダーの制御を行います。

## 対応機種

本ソフトウェアがプロファイルとして識別する機種は以下のとおりです。

| 機種 | USB ID | 受信部 | ソフトウェアによる LNB 15V 給電 |
|---|---|---|---|
| PX-S3U | `0b06:0001` | 1 つ（地デジと衛星で排他） | 非対応（要求 `15` は拒否。`0` は給電要求を出さない設定であり、端子が 0V とは限りません） |
| PX-S3U2 | `0b06:0003` | 0: 衛星、1: 地デジ（同時は 1 系統のみ） | 同上 |
| PX-W3U2 | `0b06:0004` | 地デジ 2 + 衛星 2 の計 4 受信部を公開し、4 番組同時録画をサポート | GPIO20 active-low。`--lnb-voltage 15` を受理 |
| PX-W3U3 | `0b06:0005` | 同上 | 同上 |
| PX-W3U3 V2 | `0b06:0006` | 条件付きの primary（[仕様上の制限](SPEC.md#010-source-supported-model-boundary)） | GPIO20 active-high。`--lnb-voltage 15` を受理。給電中は GPIO80 を監視 |

候補 6dbdba2 の PX-W3U3 は Linux x86_64・macOS arm64 の短時間項目を確認しましたが、BS09 の CC 異常と未完了項目があり、リリース認定は未完了です。[現在の検証状況](docs/release-validation/0.1.0.md#現在の候補と検証状況)を参照してください。PX-W3U2 の[限定試験](https://github.com/Khronos31/asicen-userland/blob/02282efbf8394721a47dca8a70b5253be36646fa/docs/reverse-engineering/w3u2-type10-2026-10-16.md)は別記録です。S3U・S3U2・V2 は実機未確認です。

PX-W3U2 と PX-W3U3 は地デジ 2 系統と衛星 2 系統の計 4 受信部で、4 番組を同時録画できます。ASICEN main `bd25092fdae7240e9fddb8a3a3d19f4dc199e9ee` に4同時録画の実装を採用済みです。W3U2での固定source実機記録と、W3U3 coldを含む機種別受入の状態は検証記録で区別します。PX-W3U3 V2は上記2機種と同じ対応範囲とは扱わず、仕様上の制限に従います。

実行時の内部経路はブリッジ revision 11/52・controller type 0f・version 7 に限定し、W3U2 は type 10 も受理します。未実装の revision 16/52 等は起動時に拒否します。機種名だけで全個体の対応を保証しません。

`asicend --models` は USB にアクセスせず、対応機種名と VID:PID だけの静的一覧を表示します。機種別の認定状態や個別の電圧実測は[検証記録](docs/release-validation/0.1.0.md#現在の候補と検証状況)で区別します。

`asicend`・`asicen-ts`・`asicenctl`・`asicen-termux` は、それぞれ単独の `--version` で製品の版を表示します。機器やデーモンの起動は不要です。`asicend -h` は `--help` と同じヘルプを表示します。

## LNB 電源制御

衛星放送（ISDB-S）の受信時に、`asicen-ts` の `--lnb-voltage 0|15` オプションで LNB 電源の要求を指定します（既定値は `0`）。px4-userland と同じく、15V を要求する場合はデーモン起動時に `--allow-lnb-power` を指定します。許可フラグがない場合の 15V 要求は拒否します。

地デジ（ISDB-T）では `--lnb-voltage` を付けません（`0` を含め、明示すると拒否されます）。

LNB 給電に関する注意点は以下のとおりです。

- **AC アダプタの要件**: PX-W3U2、PX-W3U3、PX-W3U3 V2 の公式仕様では、LNB 給電時に AC アダプタの接続が必要です。
- **端子電圧**: USB 転送や GPIO の読み戻しは電圧の実測ではありません。今回測定した W3U3 の結果を、他機種・未試験環境へ広げません。
- **PX-S3U / PX-S3U2 の制限**: 機器の物理仕様としては LNB 給電能力を持っていますが、回収された公式ソフトウェアの解析において給電の切り替え処理が存在しないため、本バックエンドでは `--lnb-voltage 15` を非対応として拒否します。PX-W3U3 の GPIO 制御を S3U 系に適用することはありません。
- **安全制御**: デーモン起動時は安全のため先に給電オフを書き込みます。デバイスのクローズ時およびデーモン終了時は、以前給電していた GPIO の状態に戻さず確実にオフにします。選局に失敗した場合は選局前の設定へ戻します。また、PX-W3U3 V2 では保護条件（GPIO20 が立ち、GPIO80 が落ちる状態）を検出すると給電を遮断し、自動でオンへ戻すことはしません。

## 動作環境

バージョン 0.1.0 のバイナリ配布は Linux・macOS・Android の 8 種類で、Windows は当時の配布物に含まれていません。現在の正式検証対象には Windows 11 x86_64 を含みますが、現行 workflow は Windows ZIP を `win-candidate` として別生成し、通常の配布aggregateへ接続していません。これは配布経路の実装差であり、正式対象外を意味しません。候補ごとの結果は[検証状況](docs/release-validation/0.1.0.md#現在の候補と検証状況)に記録します。アーカイブ名は `asicen-userland-0.1.0-` で始まります。

| 対象 OS / 環境 | 配布アーカイブ名 | ビルド方針 | 候補 6dbdba2 の実機確認 | 構成と備考 |
|---|---|:---:|:---:|---|
| Linux x86_64 (glibc) | `asicen-userland-0.1.0-linux-glibc-x86_64` | CI の対象 | E03 短時間確認・未認定 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は glibc 2.31 向け共有オブジェクト |
| Linux x86_64 (musl) | `asicen-userland-0.1.0-linux-musl-x86_64` | CI の対象 | SCS 補足のみ・E02 未認定 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は musl 向け共有オブジェクト |
| Linux aarch64 (glibc) | `asicen-userland-0.1.0-linux-glibc-aarch64` | CI の対象 | 未確認 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は glibc 2.31 向け共有オブジェクト |
| Linux aarch64 (musl) | `asicen-userland-0.1.0-linux-musl-aarch64` | CI の対象 | 未確認 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は musl 向け共有オブジェクト |
| macOS arm64 | `asicen-userland-0.1.0-darwin-arm64` | CI の対象 | E04 短時間確認・未認定 | libusb 静的リンク。実行時に読み込むのはシステムライブラリのみ。IFD は `asicen-userland-ifd.bundle`。ローダを同梱 |
| Android aarch64 | `asicen-userland-0.1.0-android-aarch64` | CI の対象 | 未確認 | Termux 用。API 24、libc++ 静的リンク、16KiB ページ。IFD・APK なし |
| Android armv7a | `asicen-userland-0.1.0-android-armv7a` | CI の対象 | 未確認 | Termux 用。API 24、libc++ 静的リンク、16KiB ページ。IFD・APK なし |
| Android x86_64 | `asicen-userland-0.1.0-android-x86_64` | CI の対象 | 未確認 | Termux 用。API 24、libc++ 静的リンク、16KiB ページ。IFD・APK なし |

候補の CI は[run 38942153016](https://github.com/Khronos31/asicen-userland/actions/runs/38942153016)で 20 jobs 成功しました。ビルド・オフライン試験は実機認定とは別です。

Windows 11 x86_64 は Phase 1（製品コマンドと CARD_* を含む同一ホスト IPC）の開発・ビルド検証対象です。WinSCard 互換 DLL / Microsoft PC/SC 登録は Phase 2 で、0.1.0 の Windows 配布・正式対応は行いません。

## 必要条件

### ファームウェア

`firmware/asicen-loader.bin`（16,384 バイト）は全機種共通の Windows 由来イメージです。通常の model 省略起動ではこのイメージを使用します。固定イメージの抽出・hash検査には既存の `scripts/extract-windows-loader-firmware.py` を使います。再配布権は未解決であり、vendor notice と由来情報を各 binary archive に含めます。

通常起動は `asicend --device usb:BUS-PORT[,BUS-PORT...] --instance TOKEN --firmware PATH` です。cold loader へ共通イメージを転送した後、同じ USB 位置に戻る runtime VID:PID から機種を決めます。既に runtime なら転送を省略します。既知の内部 Hub で相方位置を一意に確認できる機種は1位置で選択でき、確認できない場合は複数位置を指定します。serial がない ASICEN 機器を `serial:` で選択した場合は一致なしになります。Termux の `--fd` は再列挙に追随できないため、転送と起動を分けます（転送後に runtime の fd を再取得して `--firmware` なしで起動）。runtime fd の VID:PID から機種を決めます。開発用の `asicen-probe load-firmware` も引き続き利用できます。

### 実行時ライブラリ

- **Linux**: コマンド 3 本（`asicend`、`asicen-ts`、`asicenctl`）は musl による完全静的バイナリであり、libusb 1.0.30 を静的リンクしているため追加のライブラリなしで動作します。PC/SC 用の IFD は、アーカイブ名に対応した libc（glibc 2.31 向け、または musl）の共有オブジェクトです。
- **macOS**: libusb を静的リンクしており、実行時に読み込むのは macOS のシステムライブラリのみです（PC/SC ヘッダはビルド時のみ参照します）。
- **Android (Termux)**: API 24 向けにビルドされ、libc++ を静的リンクし、16KiB ページに対応しています。Termux 用のコマンドラインバイナリであり、PC/SC 用の IFD や配布用 APK はありません。
- **Windows**: Windows 11 x86_64 向けの Phase 1 コマンドです。libusb は静的リンクし、libusb DLL は同梱しません。対象 USB 機能に libusb で利用できるドライバの割り当てが必要です。ドライバの変更は自動では行いません。

## Linux の USB デバイス権限

Linux で一般ユーザーがチューナーを操作するには、USB デバイスノードへのアクセス権限が必要です。以下は udev ルールの設定例です（これは設定例であり、特定のディストリビューションで検証済みの手順を示すものではありません）。

`/etc/udev/rules.d/70-asicen-userland.rules`:
```udev
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0b06", ATTR{idProduct}=="0001", MODE="0660", GROUP="video"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0b06", ATTR{idProduct}=="0003", MODE="0660", GROUP="video"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0b06", ATTR{idProduct}=="0004", MODE="0660", GROUP="video"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0b06", ATTR{idProduct}=="0005", MODE="0660", GROUP="video"
SUBSYSTEM=="usb", ENV{DEVTYPE}=="usb_device", ATTR{idVendor}=="0b06", ATTR{idProduct}=="0006", MODE="0660", GROUP="video"
```

実行ユーザーを `video` グループに追加し、udev ルールを再読込したあと、チューナーを挿し直してください。

```sh
sudo usermod -aG video "$USER"
sudo udevadm control --reload-rules
```

## 使用例

製品コマンドは、デバイスを保持・制御するデーモン `asicend`、MPEG-TS ストリームを受信する `asicen-ts`、状態確認などを行う `asicenctl` で構成されます。ASICEN デバイスには USB シリアル番号がないため、クライアントコマンドは `--instance` で対象デーモンを指定します。PX4 の共通 IPC 実装を使い、製品識別と受信部 topology を ASICEN 用に適用しています。旧研究用の `--socket` プロトコルは使いません。

### モックでの実行

実機を使わずに動作を確認できます。モックが出力する TS は合成の null パケットです（実カードの動作ではありません）。モックは開発用の `asicend-mock` バイナリです（配布物には含まれません）。

```sh
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/asicen-userland.XXXXXX")
cleanup() {
  kill "$asicend_pid" 2>/dev/null || :
  wait "$asicend_pid" 2>/dev/null || :
  rmdir "$runtime_dir" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' INT TERM
asicend-mock --runtime-dir "$runtime_dir" --instance test &
asicend_pid=$!
ready=0
i=0
while test "$i" -lt 30; do
  if asicenctl --runtime-dir "$runtime_dir" --instance test list >/dev/null 2>&1; then
    ready=1
    break
  fi
  kill -0 "$asicend_pid" 2>/dev/null || break
  sleep 1
  i=$((i + 1))
done
test "$ready" = 1 || { echo 'asicend-mock did not become ready' >&2; exit 1; }
asicenctl --runtime-dir "$runtime_dir" --instance test list
asicen-ts --runtime-dir "$runtime_dir" --instance test \
  --receiver 1 --channel T27 --packet-count 100 --output mock.ts
```

### 実機ハードウェアでの実行

接続機器は `asicend --list-json` で確認します（USB 接続を挿し直すとアドレスが変わります。再現には `--device usb:BUS-PORT` 形式を使います）。

```sh
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/asicen-userland.XXXXXX")
cleanup() {
  kill "$asicend_pid" 2>/dev/null || :
  wait "$asicend_pid" 2>/dev/null || :
  rmdir "$runtime_dir" 2>/dev/null || :
}
trap cleanup EXIT
trap 'exit 130' INT TERM
asicend --device usb:1-2.1,1-2.2 \
  --runtime-dir "$runtime_dir" --instance w3u3 &
asicend_pid=$!
ready=0
i=0
while test "$i" -lt 30; do
  if asicenctl --runtime-dir "$runtime_dir" --instance w3u3 status >/dev/null 2>&1; then
    ready=1
    break
  fi
  kill -0 "$asicend_pid" 2>/dev/null || break
  sleep 1
  i=$((i + 1))
done
test "$ready" = 1 || { echo 'asicend did not become ready' >&2; exit 1; }
asicen-ts --runtime-dir "$runtime_dir" --instance w3u3 \
  --receiver 1 --channel T27 --packet-count 30000 --output capture.ts
asicen-ts --runtime-dir "$runtime_dir" --instance w3u3 \
  --receiver 0 --channel BS01_0 --lnb-voltage 0 --packet-count 30000 --output satellite.ts
```

USB 機能が 1 つの PX-S3U と PX-S3U2 では `--device usb:BUS-PORT` を指定します。2機能機種は、既知の内部 Hub 上で相方を一意に確認できるとき1位置で選択できます。それ以外は `--device usb:BUS-PORT1,BUS-PORT2` の形式で両方の位置を指定します。Termux ではランチャーが `--fd` を渡します。

### Android (Termux) での実行

Termux:API の `termux-usb` を利用してファイルディスクリプタを渡します。配布用 APK はありません。
Android 側の Termux:API アプリと Termux 内の `termux-api` パッケージが必要です。ランチャーが使う `setsid` は `util-linux` が提供します。`termux-usb -l` で対象を確認し、`termux-usb -r <usb-path>` で各 USB 機能のアクセス許可を取得します。ランチャーは受け渡した fd を保持してデーモンを監視し、終了時は停止・待機・後始末を行います。

ランタイムディレクトリには `/tmp` を使わないでください。Termux の `/tmp` はユーザーが書き込めない場合があり、エンドポイント作成（`make_layout`）も `--runtime-dir` 未指定時は `$XDG_RUNTIME_DIR` だけを参照します。Android では UNIX ドメインソケットのパス長上限が短いため、`$HOME` 配下などの深い階層ではなく `$PREFIX/tmp` 直下の短い固定パスを使います（px4-userland と同じ運用）。

```sh
runtime_dir="$PREFIX/tmp/a3"
mkdir -p "$runtime_dir"
chmod 700 "$runtime_dir"
asicen-termux --usb-device /dev/bus/usb/001/004 \
  --runtime-dir "$runtime_dir" --instance tuner
```

USB 機能が 2 つの機種では、`--usb-device` を primary、sibling の順で 2 回指定します。ローダ状態からの起動時は、まず別途ファームウェアを転送してから runtime を起動してください（`--fd` は再列挙に追随しないため、転送と起動を分けます）。

## 受信部とチャンネル

### チャンネル指定

px4-userland のチャンネル表記を踏襲しています。

- 地デジ（ISDB-T）: `T13`〜`T62`、または `13`〜`62`
- BS（ISDB-S）: `BS<nn>[_slot]`（例: `BS01_0`）
- CS（ISDB-S）: `CS<n>`

### キャプチャ制御オプション

- `--tune-timeout-ms`: 選局タイムアウト時間（100〜30000 ミリ秒、既定値 10000）
- `-t/--time SECONDS` と `--packet-count`: 取得時間またはパケット数を指定します。これらは同時に指定できません。
- `--output -`: 出力先に標準出力を指定した場合、MPEG-TS のみを出力します。

### 終了コード

エラー処理と終了コードの考え方も px4-userland を踏襲しています。

- `0`: 成功
- `2`: 使い方（コマンドライン引数の不正など）
- `3`: 見つからない / 未準備 / 非対応
- `4`: 使用中
- `5`: タイムアウト
- `6`: IPC
- `7`: USB
- `8`: TS
- `9`: カード
- `10`: ファームウェア
- `70`: 内部エラー

## PC/SC と内蔵カードリーダー

内蔵 IC カードリーダーを利用するための PC/SC IFD プラグインとして、`ifd/asicen-userland-ifd.so`（macOS では `ifd/asicen-userland-ifd.bundle`）を提供しています。ホストの `pcscd` が、対応する libc 向けの IFD ライブラリを読み込みます（Android には IFD はありません）。

配布物の `reader.conf.d/asicen-userland.conf` の 4 つの項目を実環境へ置き換えます。

- `@ASICEN_RUNTIME_DIR@`: デーモンとクライアントが共有する runtime root
- `@ASICEN_INSTANCE@`: デーモンの `--instance TOKEN` と同じ接続先名
- `@ASICEN_IFD_LIBRARY@`: 上記 IFD の絶対パス。macOS の `LIBPATH` は bundle ディレクトリで、内部の dylib ではありません
- `@ASICEN_ACCESS@`: 同じユーザーで動かす `user`、またはサービスユーザーと共有グループで動かす `group`

group mode では runtime root を共有グループが利用できるように設定し、デーモンとクライアントに `--group` を指定します。pcscd のサービスユーザーも共有グループに所属させます。設定を配置後、利用する PC/SC デーモンで再読み込みまたは再起動して反映します。

プライベートリーダーの設定例:

```
DEVICENAME asicen-userland:runtime=/tmp/asicen-example:instance=w3u3:access=user
```

`asicenctl` のカード操作コマンドは、未実装の操作に対して適切に失敗を返します（ATR を捏造することはありません）。

LNB 給電に対応する機種（PX-W3U2・PX-W3U3・PX-W3U3 V2）では、`asicenctl` の `lnb-on` / `lnb-off` で筐体の LNB 給電を、TUNER の acquire や選局とは独立に直接 ON/OFF できます。対象の選択は `status` と同じ `--device usb:...` / `--instance` / `--runtime-dir` を使います。ON にはデーモン起動時の `--allow-lnb-power` が必要で、許可がなければ UNSUPPORTED（終了コード 3）を返します。

独立要求は受信部の 15V 要求と合算され、どちらかが要求している間は ON を保ちます。そのため、電圧指定を省略した（または 0V の）選局、選局の失敗によるロールバック、正常な受信部の close では独立要求は解除されず、`lnb-off` も受信中の受信部の給電は切りません。デーモン停止時と、保護動作・後始末の失敗時は独立要求を破棄して OFF を試みます。切断後は GPIO を操作せず、出力状態を不明とします。

認識状態は `status` の `enclosure.lnb_15v_enabled`（true / false / null）で表示します。デーモンが GPIO の読み戻しで確認できた状態だけを true / false とし、未確認・失敗・切断は null です。端子電圧の実測値ではありません（実機での 15V 出力は未検証です）。

## 注意事項

- **同時受信ストリーム**: PX-W3U2/PX-W3U3の現行mainはreceiver 0〜3の4 laneを公開します（0/2が衛星、1/3が地デジ）。実装状態と候補ごとの実機受入結果を混同しないでください。
- **公開される受信部**: PX-W3U2/PX-W3U3は0/2を衛星、1/3を地デジに割り当てます。ほかの機種ごとの割り当ては上の表のとおりです。
- **対応ハードウェア revision**: 実行時に対応している内部経路はブリッジ revision 11 / ASIC type 0f / version 7 のみです。revision 16 は未実装のため起動前に失敗します。すべての個体がこの revision に該当するとは限りません。
- **TS 品質カウンタ**: デーモンは受信 TS の TEI・continuity・同期の統計を計数します。取得パケット数と終了状態も確認してください。値が 0 だけでは「無誤りで受信できた証明」にはなりません。
- **ハードウェアの列挙**: 接続機器の確認は `asicend --list-json` を使います。ASICEN は USB serial を持たないため、JSON の `serial` は null、識別は観測した USB topology です。
- **過去の受信記録と本配布物の位置づけ**: 過去の開発ビルドにおいて、PX-W3U3 の primary 受信部（receiver 1 の T27、receiver 0 の BS01_0）を受信し、内蔵カード経由の B25 復号まで成功した記録があります。ただし、これは過去の開発ビルドでの記録であり、今回の 0.1.0 配布候補バイナリの実機検証を証明するものではありません。

## ソースからのビルド

ビルドには、C++17 対応コンパイラ、CMake、pkg-config、libusb の開発用ヘッダが必要です。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DASICEN_ENABLE_LIBUSB=ON -DASICEN_BUILD_TESTS=ON
cmake --build build --parallel 2
(cd build && ctest --output-on-failure)
```

実機でのキャプチャには Release ビルドを推奨します。同一ソースコードを用いた比較において、最適化なしのビルドでは両レーンで TS エラーが発生したのに対し、Release ビルドではレーンあたり 30,000 パケットで TEI（Transport Error Indicator）および連続性エラーが 0 であったという開発時の記録に基づきます。ただし、この記録は本 0.1.0 配布候補の実機合格を証明するものではありません。

## リリース作業

検証する candidate と公開する tag CI の成果物を区別し、必要試験、依頼後のノート作成、別の「タグ＋リリース」依頼の順に進めます。[公開手順](docs/releasing.md)、[実機検証手順](docs/release-validation.md)、[matrix・記録形式](docs/release-record.md)、[コミット済みノートの方針](release-notes/README.md)を参照してください。公開workflowの成功は新しい機種・環境の実機認定を意味しません。

## ライセンス

本ソフトウェアのライセンスは GPLv2 です（px4 由来の GPL-2.0-only と FC0012 の GPL-2.0-or-later を含んでいるため、MIT ではありません）。
ファームウェアの権利関係は本ソフトウェアのライセンスとは別です。
詳細については [NOTICES.md](NOTICES.md) を参照してください。
