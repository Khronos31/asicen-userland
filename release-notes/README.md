# コミット済みリリースノート

版ごとの利用者向け本文を `release-notes/vX.Y.Z.md` に置きます。GitHub Release のタイトルは `vX.Y.Z`、本文はそのファイルの内容をそのまま使います。書式は [AGENTS.md](../AGENTS.md)「Release notes」、全工程は [docs/releasing.md](../docs/releasing.md) を参照してください。

## 作成する時期

- 候補固定前: 対象版の Markdown と機械可読記録 `vX.Y.Z.json` に、下記の無効な placeholder だけを置きます。候補と公開ソースの inventory を一致させる準備であり、ノート本文の作成でも承認でもありません。
- 必要な実機・仕様変更試験と再現性確認の後: 利用者がノート作成を明示的に依頼したら、Markdown を完成させます。利用者向けの変更と注意に絞り、検証 matrix や詳細ログは正本へリンクします。
- 結果・ノート・記録をコミットした後: 別の「タグ＋リリース」の依頼を受けてからタグを push します。ノート作成・commit は公開許可を意味しません。

このディレクトリの README は手順です。現在版について実際のノートや完了記録を先に作ったものではありません。必要な版の placeholder は候補準備時に作ります。

## 公開できない placeholder の例

`vX.Y.Z.md`（版を実値に置き換える前の例）:

```markdown
PENDING: Tests and the user's release-note request are not complete.
```

`vX.Y.Z.json`:

```json
{"schema": 1, "status": "pending"}
```

この JSON は完成記録の schema を満たさず、必ず公開検査に失敗します。架空の run ID、SHA、hash、合格、承認を埋めて検査を通さないでください。完成時の schema と matrix は [docs/release-record.md](../docs/release-record.md) にあります。公開本文には対応する製品・版の先頭見出しが必要であり、placeholder をそのまま公開しません。

