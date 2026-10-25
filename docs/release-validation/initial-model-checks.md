# ASICEN 初回機種チェック

この初回手順はASICENの対象全機種に適用する。各機種について代表Linux x86_64 glibcで2時間連続受信し、その機種で毎release回帰項目と必要時項目を実施する。機種ごとに結果を分け、他機種の結果を継承しない。通常の無指定時検証機種はPX-W3U3だが、初回対象の限定ではない。以下はPX-W3U3の問題発見段階の実績で、最終candidate archiveの初回認定ではない。

## PX-W3U3 実測結果

| 項目 | 実施内容と結果 | 状態 |
|---|---|---|
| 2時間連続受信 | Linux x86_64 glibc上のlocal build-qaでPX-W3U3を受信。 | 実施済み、final candidateでは未認定 |
| 最大同時受信 | 既存の2時間試験はreceiver 1の単一T27 stream。W3U3の公開slot 0/1は確認できるが、現行README記載の実装上限1本は機種本来の最大能力を確定しない。 | 最大能力未確認。単一streamの2時間実績だけでは初回の最大能力項目を満たさず、機種本来の最大に届かなければ未達 |
| 全channel短時間取得 | 地上/衛星targetを取得。独立TS解析でT30に5,359、T31に7,368 continuity discontinuities。client CC counterは0だった。 | 実施済み、TS判定保留。原因調査・再試験なし |
| CARD / APDU | status present→抜去時 absent→再挿入後ATR/APDU復帰。初期reset試行はexit 9。独立PC/SC probeは成功。 | 実施済み、reset失敗試行も記録 |
| LNB | PX-W3U3端子を0→15→0Vで実測。 | 実施済み |
| USB cold復帰 | 停止中に抜去/再挿入しcold startを試したがdaemon exit 3、sibling USB functionが出ず、T27再取得を実施できなかった。 | 未達 |
| OS/ABIのnative回帰 | 最終candidate archiveによる全配布OS/ABI確認なし。 | 未実施 |

## PX-W3U2 の既存部分実測

1.0.0 musl candidateでPX-W3U2を8秒→600秒→8秒受信した部分実測は[`1.0.0.md`](1.0.0.md)に記録する。この記録はPX-W3U2の2時間連続受信、同機種の毎release回帰項目、必要時項目を満たす初回結果ではなく、PX-W3U2初回は未完了である。PX-W3U3の初回結果にも代用しない。

PX-W3U3では公称4 tuner paths、discoveryの4 route records、公開slot 0/1の2つ、現行実装上限の同時stream 1本を別々に扱う。機種本来の最大同時能力は未確認であり、最大能力試験は未達のままとする。PX-W3U3 V2、PX-S3U、PX-S3U2など各対象機種にも同じ初回手順を適用し、結果がない機種は未実施のまま記録する。全項目完了まで機種ごとの初回チェックは未認定とする。
