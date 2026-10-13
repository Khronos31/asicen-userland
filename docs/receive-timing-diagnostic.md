# USB受信・変換時間の診断（Issue #7）

この文書は `dot/diagnostic-rx-timing` の診断用手順です。正式候補の受入試験や、
CS受信不良の修正完了を示すものではありません。

## 目的と固定条件

E03のCS2・10秒比較で、4本×4KiBのA1/A2はTEI 225/279、CC不連続7,763/8,438、
4本×64KiBのBはTEI 4、CC不連続2,721でした。Bも前半・後半にエラーが残り、
すべてclient exit 8です。解析プログラムのexit 0は正常に解析が終わった意味で、
保存TSが無誤りという意味ではありません。

次の診断ではBの4本×64KiBを維持し、USB完了通知、処理開始、入力コピー、
リンク変換とemit、再投稿の時間と、変換に渡したUSB入力を記録します。
GPIO、LNB、選局、DSC、CF、framer、同期probe、CC判定、変換、再投稿順は変えません。
共通PX4のCLI・IPCへ診断オプションを追加しません。

## 記録と情報の扱い

診断は既定で無効です。デーモン起動時だけ `ASICEN_RX_DIAGNOSTIC_PREFIX` を指定し、
新しい出力先を用意します。既存ファイルを上書きしないため、各試行で別のprefixを使います。
出力はprefixに対応する `.raw` と `.csv` です。親ディレクトリはあらかじめ利用者だけが
読める場所に作り、十分な空き容量を確認してください。
環境変数は各captureのprepare時に読みます。同じdaemonで2回目を取得しても同じprefixを
再利用できないため、今回は1回の取得ごとにdaemonを起動し直します。
prepare時には空ファイルを排他的に予約します。CSVの作成だけに失敗した場合、
新しく作られた空rawが残ることがあります。過去のファイルを自動削除して再試行しません。

- rawはリンク変換前の入力です。受信中は有界メモリへ保存し、転送のdrain完了後に書き出します。
- rawには正常完了し実際にdecoderへ渡した非空入力だけを記録します。drain、timeout、errorの
  callbackはCSVのmetadataに残しますが、そのpayloadはrawへ混ぜません。
- rawの上限は64MiB、イベントの上限は65,536件です。上限に達した診断を完全な記録として扱いません。
- 試験した64bit ABIでは1イベント240byteで、予約する記録バッファは計79MiBです。
  これはdaemon本体や既存TSキュー、stdio等を含まない追加メモリです。
- callback時刻はホストがlibusb callbackを実行した時刻です。USBバス上の到着時刻ではありません。
- payloadのコピー、時計読取り、イベント記録には観測負荷があります。記録なしの同じ診断版とも比較します。
- カード応答、APDU、リンクseed、DES鍵は記録しません。rawを平文TSや復号済みコンテンツとは扱いません。
- 生TSとrawは手元に保持します。Issueへ返すのは集計、バイナリhash、コマンド、終了コード、非機密ログです。

異常終了、書込み失敗、イベント上限、raw上限、drain失敗があれば記録の完全性を確認します。
診断ファイルがあることだけで完全な取得と判断しないでください。
CSV末尾の `finalized=1` に加え、daemonの保存成功ログと正常終了を確認します。
drain不能や強制終了では予約した空ファイルが残り、完了印のないファイルは解析対象にできません。

## 実機側の順序

実行ファイルのexact commit・Actions run/artifact・archive hashはIssue #7の引き継ぎで固定します。
以前のアーカイブと混ぜず、今回の診断版を新しいディレクトリへ展開します。

1. 既存E03/W3U3のUSB・アンテナ経路を維持する。LNB 0V、receiver 0、CS2、10秒を維持する。
2. 同じ診断版で、記録無効の対照を1回、記録有効を1回取得する。各回daemon/runtime/instanceを新しくする。
3. 記録有効では、デーモンの環境にだけ `ASICEN_RX_DIAGNOSTIC_PREFIX` を設定する。クライアントの引数は同じ。
4. client exit 8でもTSを消さず、daemonを通常のSIGTERMで終了させる。診断の書出し完了と終了コードを確認する。
5. daemon/clientログ、同じTS解析器の全体・前半/後半の結果、診断集計を残す。上限や書込み失敗があればそのまま報告する。

記録有効の起動例（`BIN`、`RT`、`INST`、`EVIDENCE`はその試行の実際の値）:

```sh
ASICEN_RX_DIAGNOSTIC_PREFIX="$EVIDENCE/rx" \
  "$BIN/asicend" --usb-path 1-3.1 --usb-path 1-3.2 --model w3u3 \
  --firmware "$BIN/firmware/asicen-loader.bin" \
  --runtime-dir "$RT" --instance "$INST"
```

記録なしの対照では環境変数をunsetし、同じ診断バイナリと同じ引数を使います。
daemon終了後、診断版のソースにある次のスクリプトをPython 3で実行します。
これはファイルの読取りとJSON出力だけで、USB操作やアップロードは行いません。

```sh
python3 scripts/analyze-rx-diagnostic.py "$EVIDENCE/rx" > "$EVIDENCE/rx-summary.json"
```

解析exit 2は記録形式・完全性の異常です。exit 0も受信品質の合格ではありません。
`raw_capture_complete`、省略件数、`framer_replay` の比較範囲・停止理由・不一致、
`timing` の処理時間、`raw_headers` の候補件数を確認します。
CSVの時刻0はその段階の記録なし、非0は初期化後のhost steady clockのns値+1です。
`observed_valid_bytes` は長さ検査を通ったcallbackの合計、`decoder_input_bytes` は実際のdecoder入力であり、
drain等の未処理分がある場合は一致しません。
`raw_capture_complete` の完全性はdecoderへ渡した入力の記録についてであり、
USB上の全データやdrain時のpayloadまで保存できたという意味ではありません。
`submission_window` は投稿前後のqueue lockの開放・再取得も含み、libusb呼出しだけの時間ではありません。
`processing_to_copy_end` もdispatchやカウンタ取得を含むため、純粋なmemcpy時間とは扱いません。

記録が問題を増やしている場合、記録有効の結果だけを通常運転の挙動として扱いません。
全チャンネル回帰、ソーク、LNBの再測定、物理的な抜差しをこの切り分けへ追加する必要はありません。

## 解釈できる範囲

Mode 7のリンク変換は188byte packetの先頭4byteを保持します。raw側でも、
framerが選んだpacketのsync、PID、TEI、AFC、CCを確認できます。一方、adaptation field内の
discontinuity indicator等はrawのままでは判断できません。rawのCC飛びを、
変換後TSの規格上のCCエラーと同一視しないでください。

rawのフレーミング再現と変換後TSのheader比較は、ソフトウェア内での境界の違いを絞るために使います。
再投稿の遅れとエラーの時間的な対応だけでは、装置FIFO溢れは証明できません。
rawですでに不整合がある場合も、RF、demod、ASIC、USBまでのどこで起きたかは別の切り分けが必要です。
この診断にはカード鍵を出力する機能も、ARIB復号を再現する機能もありません。
