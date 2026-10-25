# ASICEN 初回機種チェック

標準機種はPX-W3U3。条件は代表Linux x86_64 glibcで2時間連続受信し、同機種で毎release回帰項目と必要時項目を実施すること。以下は問題発見段階の実績で、最終candidate archiveの初回認定ではない。

| 項目 | 実施内容と結果 | 状態 |
|---|---|---|
| 2時間連続受信 | Linux x86_64 glibc上のlocal build-qaでPX-W3U3を受信。 | 実施済み、final candidateでは未認定 |
| 最大同時受信 | 現行READMEの同時stream上限は1本。上記T27受信はreceiver 1の単一stream。slot 0/1の2件を2同時streamとは解釈しない。 | 単一streamは実施済み、final archiveのcounter/終了値確認は未認定 |
| 全channel短時間取得 | 地上/衛星targetを取得。独立TS解析でT30に5,359、T31に7,368 continuity discontinuities。client CC counterは0だった。 | 実施済み、TS判定保留。原因調査・再試験なし |
| CARD / APDU | status present→抜去時 absent→再挿入後ATR/APDU復帰。初期reset試行はexit 9。独立PC/SC probeは成功。 | 実施済み、reset失敗試行も記録 |
| LNB | PX-W3U3端子を0→15→0Vで実測。 | 実施済み |
| USB cold復帰 | 停止中に抜去/再挿入しcold startを試したがdaemon exit 3、sibling USB functionが出ず、T27再取得を実施できなかった。 | 未達 |
| OS/ABIのnative回帰 | 最終candidate archiveによる全配布OS/ABI確認なし。 | 未実施 |

PX-W3U2は補助機種の別試験であり、PX-W3U3初回結果の代用ではない。全項目完了まで初回チェックは未認定とする。1.0.0の問題発見結果は[`1.0.0.md`](1.0.0.md)に記録する。
