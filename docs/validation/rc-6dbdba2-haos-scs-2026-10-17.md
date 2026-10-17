# 2026-10-17 JST: 0.1.0 候補 6dbdba2 の HAOS/SCS 補足検証

## 候補と記録の範囲

- source: `6dbdba248a4d9dc5d3f0ef9c887c6e150cb2e9cd`
- candidate ref: `candidate/0.1.0-20261017`
- [GitHub Actions run 38942153016](https://github.com/Khronos31/asicen-userland/actions/runs/38942153016)、attempt 1。候補全体の identity は [E03 記録](rc-6dbdba2-e03-2026-10-17.md) に対応する。
- 実行対象: `asicen-userland-0.1.0-linux-musl-x86_64.tar.gz`
- archive SHA-256: `371f869b974e78f4431b790a7f417f6a1b0d108579b26e43a7931eb83ad18418`。転送元候補と HAOS 側コピーの一致を確認。
- 実機: PX-W3U3、primary `1-2.1` / sibling `1-2.2`。初期状態および再接続後は loader `1738:5211` から同梱 firmware で通常初期化。
- 実行環境: HAOS kernel `6.18.52-haos`、x86_64、hostname `9bd85267-scs-forge`。実際の SSH 接続先 userspace は **Debian 13 / glibc 2.41 の SCS 環境**。3 実行ファイルは x86_64 の静的リンク。
- access path: 既存 SSH 経路から HAOS 上の SCS 環境へ接続し、物理 USB を候補の native libusb 経由で使用。Alpine/musl userspace ではない。
- 地上波 RX1、衛星 RX0、同時ストリーム 1 本。衛星受信は壁設備を使用し LNB 0 V、LNB 実測時だけ衛星同軸を分離した。

**本記録は musl 静的配布バイナリを Debian/SCS 上で動かした補足実機結果である。E02 の Alpine/musl native 実機試験や musl IFD の完了を示さない。** 実施した項目は「今回再検証」だが、所定の E02 行は「未認定・未完了」のまま。30 分ソークも実施していない。

本書は詳細の補足記録であり、sidecar の完了表明、全 release gate の完了、正式公開の承認ではない。製品コード・候補バイナリは変更せず、旧候補の合格を継承しない。

以下の実行時刻は UTC（JST は UTC+9）。利用者の発言時刻は観測結果の報告時刻であり、物理操作や電圧到達の瞬間ではない。秒単位の伝達時刻には数秒の差があり得るため、操作から検出までの遅延算出には使わない。

## 短時間受信

| 試行 | 実行日時 UTC | 指定時間 | packets | bytes | client / validator / daemon exit | 結果 |
|---|---|---:|---:|---:|---|---|
| 初回 T27 | 2026-10-16 22:57:51.284090909–22:58:52.576926707 | 60 s | 492217 | 92536796 | 0 / 0 / 未取得 | TS 品質項目に異常なし |
| 初回 BS01、LNB 0 V | 2026-10-16 22:59:19.536277447–23:00:20.047024477 | 60 s | 535094 | 100597672 | 0 / 0 / 未取得 | TS 品質項目に異常なし |
| USB 再接続後 T27 | 2026-10-17 00:06:35–00:07:36 | 60 s | 661286 | 124321768 | 0 / 0 / 0 | TS 品質項目に異常なし |

T27 は RX1 / ISDB-T / 557142 kHz、期待 TSID 32736。BS01 は RX0 / ISDB-S / 1049480 kHz / stream 16400 / LNB 0 V。ISDB-T では `--stream-id` を指定せず、TSID は取得データ側で照合した。

初回 2 取得は既存 `ts_validation.py` で offline 検査し、TSID 一致、invalid packet・sync・TEI・continuity discontinuity/duplicate/reset は 0。native の USB errors・queue drops・empty intervals も 0。実行開始から終了までの壁時計差は T27 約61.293秒、BS01 約60.511秒で、選局等を含む。TS bytes は packets×188 と一致した。pump `copied_bytes` は出力 TS より T27 で36 bytes、BS01 で88 bytes多かったが、overflow/discard は 0。

初回 daemon は detached 起動のため親の wait status を回収できなかった。SIGTERM コマンドの exit 0、送信後約1秒で PID 消失、最終プロセス不在を確認したが、**daemon 自身の exit 0 とは記録しない**。

復帰 T27 は client を包む timeout の wait exit 0、validator exit 0。TSID 一致、CC discontinuities/resets/duplicates/conflicts・TEI・sync はすべて 0。native USB errors・queue drops・empty intervals、pump ingress/submission overflow は 0、accounting は reconciled。daemon は同じ親シェルで SIGTERM 後 wait exit 0。watchdog 停止、残存プロセス・socket なしを確認した。

## LNB 実測

衛星同軸を外し、利用者がテスターを当てて測定した。初期および明示 0 V 指定後に 0 V の報告を受けた。OFF 試行は BS01 RX0 / 1049480 kHz / stream 16400 / `--tune-timeout-ms 30000` / `--duration-seconds 1` / 出力 `/dev/null` を使用した。

| 操作 | 実行日時 UTC | client exit / 結果 |
|---|---|---|
| 15 V 指定、1回 | 2026-10-16 23:25:58–23:26:28 | 5 / TIMEOUT、0 packets / 0 bytes |
| 続いて明示 0 V | 2026-10-16 23:26:37–23:27:07 | 5 / TIMEOUT、0 packets / 0 bytes |

無信号の選局タイムアウトであり、`duration-seconds 1` は15 V維持時間ではない。実行は約30秒の tune timeout 窓だった。利用者は15 Vを約30秒維持した後に下降中と報告した。後続の報告は、23:27:56 UTC頃に0.5 V、23:39:11 UTC頃に0 V。これらはチャットの報告時刻であり、0 Vへの到達時間、減衰時定数、容量、部品健全性を算定しない。

daemon は同じ親シェルから SIGTERM し wait exit 0。残存 daemon/client/socket なし。初回の15 V実行依頼は実行前に止まっており、その後の単発試行を別ディレクトリに記録した。「15 V未実行」および「各1秒」の途中説明は、後の保存済み時刻・終了値照合によって訂正されている。未実行だった依頼を追加の給電試行には数えない。

## 同一 daemon でのカード抜き挿し

- daemon PID `2188604`、readiness 2026-10-16 23:40:35 UTC。
- 初期 presence `yes`、ATR取得 exit 0、有効長13 bytes。
- 利用者の抜去後、23:45:20 UTC の `card-status` は exit 0、カード不在。同じ daemon の生存を確認。
- 再挿入後、同じ daemon へ `card-status` と `card-atr` を各1回実行し、どちらも exit 0。ATRは形式有効、13 bytes。
- **再挿入後の presence 値だけは記録用 parser の構文誤りで未記録。** 保存済みの生 `card-status` 出力もなく、値を復元・推定しない。ATR復帰の成功とは区別する。
- daemon の再起動を挟まず、終了時は同じ親シェルで SIGTERM / wait exit 0。watchdog 停止、残存プロセス・socket なし。

記録は presence、取得成否、有効性、長さに限定し、生ATR・APDU・カード識別値は含めない。native CARD_* CLI の結果であり、PC/SC/IFD 経路の追加認定には使わない。

## USB切断と新 daemon での復帰

T27 の実受信開始は2026-10-16 23:51:32 UTC。TSが非ゼロかつ増加することを確認後、利用者がUSBを取り外した。抜去完了の報告は23:53:30 UTC頃で、厳密な抜去時刻は未計測。

- client を包む timeout PID `2191983` の wait exit 7。`asicen-ts` PID `2191985` は終了済み、stderr は `DISCONNECTED`。23:54:04 UTC時点で終了を確認。
- daemon PID `2191625` は切断に伴い自発終了し、23:54:27 UTCに wait exit 7を記録。SIGTERMによる正常停止ではない。ログに `USB_IO` / `LIBUSB_ERROR_NO_DEVICE`。
- TS: 1117249 packets / 210042812 bytes。既存 validator exit 0、TSID32736一致、sync・TEI・CC discontinuities/resets/duplicates/conflicts はすべて0。
- watchdog 停止、残存チューナープロセス・socket なし。

23:57:21 UTC頃の初回再接続報告後、候補 `--list-json` は2回とも exit 0で空。00:00:33 UTCの読み取り確認では、OSの `lsusb` と sysfs にも期待する W3U3 と `1-2` / `1-2.1` / `1-2.2` がなかった。別機種の列挙をW3U3復帰に数えない。OS列挙前の問題であり、対象機器のdriver binding状態は評価できない。

利用者の再挿し直し報告（2026-10-17 00:02:14 UTC頃）後、00:02:47 UTCに両ポートの loader `1738:5211` を確認した。利用者の「このポートは時々こうなる」という観察を保持するが、ポート故障が原因とは確定しない。loader があるのに runtime enclosure が未表示という理由だけで初期化前提不足とした途中判断は訂正。候補の通常 daemon 起動、同梱 firmware 転送、両ポート claim から readiness に到達し、上表の60秒復帰試験に成功した。旧 daemon の自動復帰ではない。

## 無効試行と記録処理の制限

- USB切断準備の最初の client は ISDB-T に `--stream-id` を付けたため引数検査 exit 2、0 bytes。受信品質の観測には数えず、同じdaemonで引数を訂正した後の実受信と分ける。
- LNB OFF準備で readiness を stdoutだけから読んだ値は0だった。実際の `asicend ready` は stderr にあり、stderr・socket・PIDを照合してからclientを実行した。製品の準備失敗とは扱わない。
- 初回短時間受信のdaemon数値exit欠落、再挿入presence欠落を後の成功で埋めない。
- 復帰段階では以前の保持シェルが終了していたため、新しい親シェル内で起動とwaitを揃えた。過去PIDのwait値を再現したという意味ではない。
- 本書は保存済み結果報告の要約であり、全 raw log の再取得・再解析を行った記録ではない。個別実行ファイルやvalidatorの完全なhash、依存ライブラリ版が本書に未掲示でも推測で補わない。

## 証拠の所在

HAOS/SCS 配下の基点:
`/config/.work/asicen-0.1.0/rc-38942153016/`

- `x/`: 上記候補の展開物
- `run-short-20261016T225314Z/`: T27 / BS01のcommands、時刻、exit、TS、validator JSON、readiness / cleanup
- `lnb-off-20261016T231455Z/`: 初期OFF段階
- `lnb-15v-once-20261016T232445Z/`: 単発15 V、後続OFF、停止記録、`lnb-meter-user-reports.txt`
- `card-cycle-20261016T234007Z/`: `card-sanitized-summary.txt`、`card-presence-removal-check.txt`、再挿入と停止
- `usb-disconnect-t27-20261016T235004Z/`: 引数誤りの試行と実受信を分けた切断記録
- `t27-recovery-60s-20261016T235815Z/`: 初回不在、再挿し直し後のloader列挙
- `t27-recovery-60s-20261017T000535Z/`: 新daemonによる60秒復帰と正常停止

生TS・カード生データ・認証情報は本書に同梱しない。

## 未完了の範囲

Alpine/musl native E02、musl IFD経路、30分ソークは未完了。別環境・別アーカイブの合格には代用しない。E03のBS09における既存CC異常や候補全体の再現性・公開前照合は本試験で解消しない。追加ソークや物理操作を本書によって開始せず、既存の利用者判断と順序を維持する。
