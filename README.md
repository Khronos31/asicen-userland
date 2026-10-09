# asicen-userland

`asicen-userland` は、ASICEN チップを搭載した PLEX 製 USB チューナーに対応した、ユーザー空間で動作するドライバおよびツール群です。カーネルモジュールを使用せず、ユーザー空間から選局、MPEG-TS ストリームの取得、および内蔵 IC カードリーダーの制御を行います。

## 対応機種

本ソフトウェアがプロファイルとして識別する機種は以下のとおりです。

| 機種 | USB ID | 受信部 | ソフトウェアによる LNB 15V 給電 |
|---|---|---|---|
| PX-S3U | `0b06:0001` | 1 つ（地デジと衛星で排他） | 非対応（要求 `15` は拒否。`0` は給電要求を出さない設定であり、端子が 0V とは限りません） |
| PX-S3U2 | `0b06:0003` | 0: 衛星、1: 地デジ（同時は 1 系統のみ） | 同上 |
| PX-W3U2 | `0b06:0004` | 物理仕様は地デジ 2 + 衛星 2。バックエンドは primary の 2 系統（0: 衛星、1: 地デジ）のみ公開 | GPIO20 active-low。`--lnb-voltage 15` を受理 |
| PX-W3U3 | `0b06:0005` | 同上 | 同上 |
| PX-W3U3 V2 | `0b06:0006` | 条件付きの primary。詳細は [docs/model-support.md](docs/model-support.md) を参照 | GPIO20 active-high。`--lnb-voltage 15` を受理。給電中は GPIO80 を監視 |

本リポジトリの 0.1.0 配布候補において、これらの機種に対する実機動作確認はまだ行われていません（実機未確認です）。

4 チューナー機（PX-W3U2、PX-W3U3、PX-W3U3 V2）は sibling 側の USB 機能も認識して確保しますが、現行のバックエンドが公開している受信部は primary 側の 2 つ（0: 衛星、1: 地デジ）のみです。同時受信可能なストリームは 1 本であり、製品カタログ上の 4 系統同時受信には対応していません。

実行時に対応している内部経路はブリッジ revision 11 / ASIC type 0f / version 7 のみです。revision 16 は未実装のため、デーモン起動前に失敗します（すべての個体が該当の revision であるとは限りません）。

`asicend --models` は USB デバイスにアクセスせず、ソースコード上の定義プロファイル一覧を表示します。表示される `lnb_validation=pending` は、アンテナ端子の電圧が未測定であることを示しています。詳細は [docs/model-support.md](docs/model-support.md) を参照してください。

## LNB 電源制御

衛星放送（ISDB-S）の受信時に、`asicen-ts` の `--lnb-voltage 0|15` オプションで LNB 電源の要求を指定します（既定値は `0`）。px4-userland にある `--allow-lnb-power` オプションはなく、デーモン側に追加の許可フラグを指定する必要はありません。

地デジ（ISDB-T）では `--lnb-voltage` を付けません（地デジに対して `--lnb-voltage 15` を指定すると拒否されます）。

LNB 給電に関する注意点は以下のとおりです。

- **AC アダプタの要件**: PX-W3U2、PX-W3U3、PX-W3U3 V2 の公式仕様では、LNB 給電時に AC アダプタの接続が必要です。
- **端子電圧の未測定**: USB 転送の成功や GPIO の読み戻しは、アンテナ端子の電圧ではありません。この配布候補では、テスターによる測定はまだです。
- **PX-S3U / PX-S3U2 の制限**: 機器の物理仕様としては LNB 給電能力を持っていますが、回収された公式ソフトウェアの解析において給電の切り替え処理が存在しないため、本バックエンドでは `--lnb-voltage 15` を非対応として拒否します。PX-W3U3 の GPIO 制御を S3U 系に適用することはありません。
- **安全制御**: デーモン起動時は安全のため先に給電オフを書き込みます。デバイスのクローズ時およびデーモン終了時は、以前給電していた GPIO の状態に戻さず確実にオフにします。選局に失敗した場合は選局前の設定へ戻します。また、PX-W3U3 V2 では保護条件（GPIO20 が立ち、GPIO80 が落ちる状態）を検出すると給電を遮断し、自動でオンへ戻すことはしません。

詳細は [docs/lnb-control.md](docs/lnb-control.md) を参照してください。

## 動作環境

バージョン 0.1.0 向けに以下の 8 種類のバイナリアーカイブを配布予定です（現時点で GitHub Release およびタグは未作成です）。アーカイブ名は `asicen-userland-0.1.0-` で始まります。

| 対象 OS / 環境 | 配布アーカイブ名 | 候補のビルド | 本候補での実機確認 | 構成と備考 |
|---|---|:---:|:---:|---|
| Linux x86_64 (glibc) | `asicen-userland-0.1.0-linux-glibc-x86_64` | このコミットのランナー成果物 | 未確認 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は glibc 2.31 向け共有オブジェクト |
| Linux x86_64 (musl) | `asicen-userland-0.1.0-linux-musl-x86_64` | このコミットのランナー成果物 | 未確認 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は musl 向け共有オブジェクト |
| Linux aarch64 (glibc) | `asicen-userland-0.1.0-linux-glibc-aarch64` | このコミットのランナー成果物 | 未確認 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は glibc 2.31 向け共有オブジェクト |
| Linux aarch64 (musl) | `asicen-userland-0.1.0-linux-musl-aarch64` | このコミットのランナー成果物 | 未確認 | コマンド 3 本は musl 完全静的リンク。PC/SC IFD は musl 向け共有オブジェクト |
| macOS arm64 | `asicen-userland-0.1.0-darwin-arm64` | このコミットのランナー成果物 | 未確認 | libusb 静的リンク。実行時に読み込むのはシステムライブラリのみ。IFD は `ASICEN-IFD.bundle`。ローダを同梱 |
| Android aarch64 | `asicen-userland-0.1.0-android-aarch64` | このコミットのランナー成果物 | 未確認 | Termux 用。API 24、libc++ 静的リンク、16KiB ページ。IFD・APK なし |
| Android armv7a | `asicen-userland-0.1.0-android-armv7a` | このコミットのランナー成果物 | 未確認 | Termux 用。API 24、libc++ 静的リンク、16KiB ページ。IFD・APK なし |
| Android x86_64 | `asicen-userland-0.1.0-android-x86_64` | このコミットのランナー成果物 | 未確認 | Termux 用。API 24、libc++ 静的リンク、16KiB ページ。IFD・APK なし |

公開 CI のオフライン試験では、マージ時点の Linux（Ubuntu）ctest が 53/53、macOS arm64 の ctest が 51/51 でした。macOS の再リンク試験は、ファームを足す前の中間ビルドに対するものです。配布する macOS アーカイブには、その後でピン留めしたローダを同梱します。これらはオフラインの結果であり、実機での受信を確認したものではありません。

Windows はバージョン 0.1.0 の対象外です。

## 必要条件

### ファームウェア

S3U / S3U2 / W3U2 / W3U3 用の Linux ローダは `firmware/asicen-loader.bin`（16,384 バイト、SHA-256 `b45d510200a1690b3ca358d93de13f40e1d3567b663c17e773349ad96f597aa8`）としてリポジトリにあります。再配布権は未解決のままです。GitHub Actions のランナーは、このチェックアウトされたファイルを 8 つの配布アーカイブへ同梱します。対応ソースのアーカイブには入れません。

PX-W3U3 V2 用の別イメージ（詳細は [docs/model-support.md](docs/model-support.md)）は、このファイルとは別物で、リポジトリには入っていません。

ファームウェアの転送は `asicend --hardware --firmware PATH` で行います（ローダ状態のデバイスへ転送し、runtime の再列挙を待ちます。既に runtime の場合は転送をスキップします）。`--model` を併せて指定してください。開発用の `asicen-probe load-firmware` も引き続き利用できます。

### 実行時ライブラリ

- **Linux**: コマンド 3 本（`asicend`、`asicen-ts`、`asicenctl`）は musl による完全静的バイナリであり、libusb 1.0.30 を静的リンクしているため追加のライブラリなしで動作します。PC/SC 用の IFD は、アーカイブ名に対応した libc（glibc 2.31 向け、または musl）の共有オブジェクトです。
- **macOS**: libusb を静的リンクしており、実行時に読み込むのは macOS のシステムライブラリのみです（PC/SC ヘッダはビルド時のみ参照します）。
- **Android (Termux)**: API 24 向けにビルドされ、libc++ を静的リンクし、16KiB ページに対応しています。Termux 用のコマンドラインバイナリであり、PC/SC 用の IFD や配布用 APK はありません。

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

製品コマンドは、デバイスを保持・制御するデーモン `asicend`、MPEG-TS ストリームを受信する `asicen-ts`、状態確認などを行う `asicenctl` で構成されます。ASICEN デバイスには USB シリアル番号がないため、クライアントコマンドは `--instance` で対象デーモンを指定します（ソケットの通信形式は px4-userland とは異なります）。

### モックでの実行

実機を使わずに動作を確認できます。モックが出力する TS は合成の null パケットです（実カードの動作ではありません）。

```sh
mkdir -m 700 /tmp/asicen-example
asicend --mock --runtime-dir /tmp/asicen-example --instance test
asicenctl --runtime-dir /tmp/asicen-example --instance test list
asicen-ts --runtime-dir /tmp/asicen-example --instance test \
  --receiver 1 --channel T27 --packet-count 100 --output mock.ts
```

### 実機ハードウェアでの実行

接続機器は `asicend --list-json` で確認します（USB 接続を挿し直すとアドレスが変わります。再現には `--usb-path BUS-PORT` 形式を使います）。

```sh
asicend --hardware --usb-path 1-2.1 --usb-path 1-2.2 \
  --runtime-dir /tmp/asicen-example --instance w3u3
asicen-ts --runtime-dir /tmp/asicen-example --instance w3u3 \
  --receiver 1 --channel T27 --packet-count 30000 --output capture.ts
asicen-ts --runtime-dir /tmp/asicen-example --instance w3u3 \
  --receiver 0 --channel BS01_0 --lnb-voltage 0 --packet-count 30000 --output satellite.ts
```

USB 機能が 1 つの PX-S3U と PX-S3U2 では `--usb-path` を 1 つだけ指定します。USB 機能が 2 つの PX-W3U2、PX-W3U3、PX-W3U3 V2 では primary（先）と sibling（後）の 2 つを指定します。Termux ではランチャーが `--fd` を渡します。

### Android (Termux) での実行

Termux:API の `termux-usb` を利用してファイルディスクリプタを渡します。配布用 APK はありません。

```sh
asicen-termux --usb-device /dev/bus/usb/001/004 \
  --runtime-dir /tmp/asicen-example --instance tuner
```

USB 機能が 2 つの機種では、`--usb-device` を primary、sibling の順で 2 回指定します。`asicend` は `--firmware` 引数を受け付けないため、起動コマンドに `--firmware` は付けません。

## 受信部とチャンネル

### チャンネル指定

px4-userland のチャンネル表記を踏襲しています。

- 地デジ（ISDB-T）: `T13`〜`T62`、または `13`〜`62`
- BS（ISDB-S）: `BS<nn>[_slot]`（例: `BS01_0`）
- CS（ISDB-S）: `CS<n>`

### キャプチャ制御オプション

- `--tune-timeout-ms`: 選局タイムアウト時間（100〜30000 ミリ秒、既定値 10000）
- `--duration-seconds` と `--packet-count`: 取得時間またはパケット数を指定します。これらは同時に指定できません。
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

コマンド体系と適合性の詳細は [docs/cli-adaptation.md](docs/cli-adaptation.md) を参照してください。

## PC/SC と内蔵カードリーダー

内蔵 IC カードリーダーを利用するための PC/SC IFD プラグインとして、`libifd-asicen.so`（macOS では `ASICEN-IFD.bundle`）を提供しています。ホストの `pcscd` が、対応する libc 向けの IFD ライブラリを読み込みます（Android には IFD はありません）。

プライベートリーダーの設定例:

```
DEVICENAME asicen-userland:runtime=/tmp/asicen-example:instance=w3u3:access=user
```

`asicenctl` のカード操作コマンドは、未実装の操作に対して適切に失敗を返します（ATR を捏造することはありません）。

## 注意事項

- **同時受信ストリームの制限**: 同時に受信できるストリームは 1 本のみです。4 チューナー機であっても、バックエンドが公開している受信部は primary 側の 2 系統（0: 衛星、1: 地デジ）のみであり、製品カタログにあるような 4 系統同時受信には対応していません。
- **公開される受信部**: 機種ごとの割り当ては上の表のとおりです。4 チューナー機で公開しているのは primary の 2 系統だけです。
- **対応ハードウェア revision**: 実行時に対応している内部経路はブリッジ revision 11 / ASIC type 0f / version 7 のみです。revision 16 は未実装のため起動前に失敗します。すべての個体がこの revision に該当するとは限りません。
- **TS 品質カウンタ**: デーモンが出力する TS 品質カウンタは未計測です。値が 0 であっても「無誤りで受信できた証明」とはみなせません。
- **ハードウェアの列挙**: 接続機器の確認は `asicend --list-json` を使います。`--list`（テキスト形式）は未実装です。
- **過去の受信記録と本配布物の位置づけ**: 過去の開発ビルドにおいて、PX-W3U3 の primary 受信部（receiver 1 の T27、receiver 0 の BS01_0）を受信し、内蔵カード経由の B25 復号まで成功した記録があります。ただし、これは過去の開発ビルドでの記録であり、今回の 0.1.0 配布候補バイナリの実機検証を証明するものではありません。

## ソースからのビルド

ビルドには、C++17 対応コンパイラ、CMake、pkg-config、libusb の開発用ヘッダが必要です。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DASICEN_ENABLE_LIBUSB=ON
cmake --build build --parallel 2
(cd build && ctest --output-on-failure)
```

実機でのキャプチャには Release ビルドを推奨します。同一ソースコードを用いた比較において、最適化なしのビルドでは両レーンで TS エラーが発生したのに対し、Release ビルドではレーンあたり 30,000 パケットで TEI（Transport Error Indicator）および連続性エラーが 0 であったという開発時の記録に基づきます。ただし、この記録は本 0.1.0 配布候補の実機合格を証明するものではありません。

## ライセンス

本ソフトウェアのライセンスは GPLv2 です（px4 由来の GPL-2.0-only と FC0012 の GPL-2.0-or-later を含んでいるため、MIT ではありません）。
ファームウェアの権利関係は本ソフトウェアのライセンスとは別です。
詳細については [NOTICES.md](NOTICES.md) を参照してください。
