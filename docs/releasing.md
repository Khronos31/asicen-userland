# Stable リリース手順

この文書は asicen-userland の候補固定、検証、リリースノート作成、タグと GitHub Release の公開手順です。判定条件の正本は `SPEC.md` と `docs/release-validation.md` の ASICEN 固有条件、実機手順は [release-validation.md](release-validation.md)、記録形式は [release-record.md](release-record.md) を参照してください。この手順の実装・PR・CI 成功は、実機合格や公開の承認ではありません。

## 共通の順序と利用者の判断

- **A: 合格した場合** — candidate → 試験結果の文書コミット → 依頼後にノート作成・コミット → 両方を含むコミットに、別の依頼後にタグ → 自動照合・Release 公開。
- **B: 失敗してコード修正が必要な場合** — 失敗を記録 → コード修正 → 新 candidate → 必要な検証を繰り返し、合格してから A へ進む。修正前の候補の合格や旧 hash を新候補へ流用しません。

候補前の placeholder はファイルの置き場所を確保する準備だけです。ノート本文や試験結果を先に書くものではありません。

1. 候補を固定し、CI の候補アーティファクトを検証します。
2. 必要な実機試験・仕様変更試験・ユーザーが決めた soak と再現性確認を完了します。
3. 試験結果を文書へ記録してコミットします。
4. 利用者から明示的に「リリースノートを書いて」と依頼された後、利用者向けのノートと機械可読記録を確定し、文書だけの後続コミットにします。
5. 利用者から別途「タグ＋リリース」の依頼を受けた後、試験結果とノートの両コミットを含むコミットに注釈付きタグを作成して push します。
6. タグを契機に、タグ CI の成果物を照合し、Draft の添付確認、公開、公開後確認まで workflow が実行します。

試験完了、ノート作成の依頼、ノートのコミットは「タグ＋リリース」の許可を代替しません。タグ push が公開工程を開始するため、承認前にタグを push しません。コミット・push・マージも、その操作についての利用者の依頼に従います。追加の GitHub Environment 手動承認は設けません。

## 公開アーティファクト

公開対象は `build_userland.yml` の成功した **tag push run** が生成した `release-candidate` です。実機試験に使った候補 run とは別の run であり、候補をそのまま添付する手順ではありません。Windows 11 x86_64も正式対象で、Windows ZIPは通常の `release-candidate` aggregateに含めます。

- バイナリアーカイブ 9 本
- `asicen-userland-<version>-source.tar.gz` 1 本
- 外側 `SHA256SUMS` 1 本（10 アーカイブの SHA-256）

現行workflowは合計11ファイルを生成します。具体的な正式 target は [release-validation.md](release-validation.md) の配布 matrix を正本とし、Windows 11 x86_64を含む9 binary targetです。0.1.0 の実配布 inventory は Linux・macOS・Android の8 binary targetで、Windows binaryはその版に含まれません。firmware は既存の pinned loader を binary archive に含め、source archive から除きます。再配布権が未解決である旨の notice を維持します。

## 1. 候補の準備と固定

対象の `VERSION` とブランチ（現在は `release/0.1.0`）を確認します。バージョンを変更する判断は別途行い、この手順の追加だけでは変更しません。

候補を作る前に、対象版の `release-notes/vX.Y.Z.md` と `release-notes/vX.Y.Z.json` を未完成の置き場所として用意します。[release-notes/README.md](../release-notes/README.md) の無効な placeholder を使い、変更内容・合格・承認を先回りして書きません。ソースアーカイブの inventory も照合するため、これらのファイルを試験後に新規追加せず、最初から候補へ含めます。

候補の full source SHA を固定し、`build_userland.yml`（`portable userland foundation`）を実行します。この build workflow 自体はタグや Release を作成しません。

```sh
gh workflow run build_userland.yml --ref <candidate-ref>
gh run view <candidate-run-id> --json headSha,event,conclusion
gh run download <candidate-run-id> --name release-candidate --dir <new-empty-dir>
(cd <new-empty-dir> && sha256sum -c SHA256SUMS)
```

実際の ref/run ID/空ディレクトリを使います。実機試験に使う archive 名・hash・source SHA・run ID/attempt を固定し、ローカル build や別 run へ置き換えません。CI の全必須 job、archive audit、source/relink、各 platform smoke が成功していることを確認します。

## 2. 実機試験・仕様変更試験と再現性

[release-validation.md](release-validation.md) の既存条件で、正式matrixの全9 binary targetを各 OS/architecture の実機で確認します。Windows を含む正式対象の実機受入が未確認ならmatrix完了とは扱いません。model、host、access path、feature は独立の軸です。他機種・他 OS・他 ABI の成功や過去の候補で穴を埋めません。物理 USB/card/アンテナ/電源操作は利用者が行い、承認された範囲だけ試験します。

同じ source SHA/tree の独立した 2 回の clean candidate run を取得し、Windows ZIPを含む正式対象9 binary archiveとsource archive（計10 archive）、および外側 `SHA256SUMS` が byte-identical であることを確認します。run ID は別々でなければなりません。実効 build input/toolchain と runner image の観測値を両方残します。runner image が異なっても実効 build input の一致を確認できればよく、入力不明や入力差は合格にしません。UUID・署名・timestamp 等をマスクした比較で代替しません。

証拠の本文は [既存の実測記録](../HARDWARE-VALIDATION.md) へ日付付きで追記します。失敗試行、未認定、対象外、継承の根拠を残し、機械可読記録の `completed` を実測の代わりにしません。必須条件が未完了ならここで止めます。

## 3. 利用者の依頼後にノートと記録を確定

全必要試験を完了し、試験結果の文書コミットを作った後、利用者がノート作成を依頼した場合に限り、`release-notes/vX.Y.Z.md` の placeholder を利用者向け Markdown に置き換えます。ノートは後続の別コミットにします。タイトル・本文の書式は `AGENTS.md`「Release notes」に従います。内部監査ログや matrix を本文へ複製しません。事実誤りや書式違反があれば同じ執筆担当へ差し戻して直します。

`release-notes/vX.Y.Z.json` には [記録形式](release-record.md) に従って、ノートの SHA-256、候補と再現性確認の run 情報、全アセットの hash、実機・仕様変更試験・soak 判断・support 表示・公開前レビューの証拠を記録します。証拠が揃わない gate は `completed` にしません。soak の実施有無・時間・対象の判断は利用者のものであり、workflow はその妥当性や物理測定を自動認定しません。

許可された結果コミットを対象ブランチへ取り込み、候補からの差分が文書だけであることを確認します。ソースのファイル構成・type・mode・symlink target を維持し、コード、script、workflow、build input、firmware を変えません。機械可読記録やノートも source inventory の一部です。これらを追加し忘れた場合は inventory の関門を緩めず、新 candidate と必要な再確認へ戻ります。

## 4. 別の「タグ＋リリース」依頼後にタグを push

利用者が対象版と結果コミットについてタグ付け・公開を依頼した後だけ、未使用の `vX.Y.Z` を作成します。以下の `<result-commit>` は試験結果コミットと後続のノートコミットの両方を含む最終コミットです。`VERSION` とタグの版を一致させ、既存タグを移動・上書きしません。

```sh
git diff --stat <candidate-commit> <result-commit>
git tag -a vX.Y.Z -m "asicen-userland vX.Y.Z" <result-commit>
git push origin vX.Y.Z
```

タグは注釈付き・署名なしです。タグ push により `build_userland.yml` の tag run と、公開用 `release_tag.yml` が始まります。通常の branch/PR CI や `workflow_dispatch` は Release を公開しません。

## 5. 自動照合、Draft、公開

0.1.0 の `release_tag.yml` は、同じ ASICEN tag commit にあるローカルの `release_reusable.yml` を使用します。PX4 のレビュー済み共通 publisher/verifier とテストを GPL-2.0-only のまま一時的に内包し、取得元・固定 commit・hash・必要な差分を [出典](../THIRD_PARTY_NOTICES.md#embedded-release-automation) に記録します。両 job の helper checkout は ASICEN 自身の exact tag SHA に一致させ、別 repository や異なる SHA を拒否します。テストも ASICEN の対象 commit を checkout し、PX4 の外部 checkout・cross-repository reusable workflow・`referenced_workflows` の外部エントリには依存しません。

PX4 v0.2.1 の正式 tag commit `dd1c1bea16a5871085d5f2a42c009364c1ec7345` に共通自動化が含まれましたが、その固定版の publisher には compare API path と Actions artifact の Accept header の不具合が残っています。ASICEN 0.1.0 は既存のローカル同梱を維持し、[出典](../THIRD_PARTY_NOTICES.md#embedded-release-automation) に明示した 2 点だけを製品側の修正として候補固定前に反映します。未リリース PX4 の外部参照や、一度限りの PX4 リリース復旧処理は追加しません。将来の外部参照への切替では、必要な修正を含む正式リリースの full commit SHA を確認し、workflow と helper を同じ SHA へ揃えます。切替自体はコード変更として新候補・必要な再確認の前に行い、既存候補への文書だけの後続変更に混ぜません。追加の token や Environment 承認は設けません。

共通 workflow は、同じ repository、正確な tag SHA/ref、`build_userland.yml`、`push` に対応した成功 run を確認します。候補/再現性 run についても repository、workflow、SHA、attempt、成功状態と実アーティファクトを照合します。run 未完了・失敗、artifact 期限切れ・欠落、入力不一致は公開を止める理由です。

照合では、候補 2 run の完全 byte 一致と、候補対 tag archive の展開後 inventory/type/mode/symlink target を確認します。実行ファイル・共有ライブラリ・ランチャー等の文書以外の payload は byte-identical が必須です。許容差は更新文書、文書と commit/tree に由来する明示的な manifest 項目、内側 checksum に限定します。未知の metadata 変更や未分類の差を見逃す allowlist は追加しません。

ASICEN Linux archive の既存 `evidence/linux-static-build.json` と `evidence/linux-ifd-build.json` についてだけ、候補/タグの SHA と一致する `source_ref` の差と、その差に対応する manifest hash/size・checksum を許します。ほかの全 field は固定し、記録された実行ファイル・IFD の hash も実 bytes と照合します。firmware は例外に含めません。

最初の検証 job は read-only です。公開 job だけが `contents: write` を持ち、再照合してから以下を実行します。

1. `vX.Y.Z` をタイトル、コミット済み Markdown の正確な内容を本文とする Draft を作成し、tag run の 10 アセットを添付
2. Draft から新しい空ディレクトリへ再取得し、全ファイルの byte 一致、10 件の checksum、タイトル・本文を確認
3. Draft を解除して Stable/Latest として公開
4. 公開済みアセットを別の空ディレクトリへ再取得し、同じ照合と Latest 状態を確認

既存の同一 Release は一致を確認して再実行を継続できますが、異なる本文・アセット・タグを上書きして合わせません。不一致では停止し、原因と現状を報告します。

## 6. 結果の確認と記録

公開 workflow の成功だけでなく、その出力から tag SHA、candidate/reproduction/tag run ID、inventory/hash、Draft/公開後照合、Release URL と Latest 状態を確認します。公開後照合が失敗した場合は「公開されたが検証未完了」と区別し、成功と報告しません。公開済みタグやアセットを修正のために置き換えません。

公開日時、照合結果、ログの所在を [実測記録](../HARDWARE-VALIDATION.md) に追記し、commit/push は利用者の依頼に従います。この後続記録のためにタグを移動せず、公開物は固定します。

