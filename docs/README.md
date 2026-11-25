# ドキュメント / Documentation

- [日本語ガイド](ja/index.md): 導入、機能、キー操作、設定、開発手順。
- [English guides](en/index.md): Getting started, features, keybindings, configuration and development.
- [コーディング規約 / Coding rules](CODING_RULES.md): 本体コードの規約。
- [P0テスト領域の管理 / P0 scratch-area policy](p0-test-area-policy.md): テスト用ファイルの作成と清掃。
- [端末の手動確認 / Manual terminal smoke test](p0-manual-terminal-smoke.md): P0端末テストの実行手順。

現在の操作と制限は日英のガイドを参照してください。P0の手順は開発者向けのテスト用です。

Use the language guides for current behavior and limitations. P0 procedures are for developers running tests.

## 文書の更新と検査 / Updating and checking guides

利用者向けの変更は日英を同時に更新し、動作・既定値・上限はソースとテストに照合してください。
手動検証の準備や自動テストの成功を、実機で確認した結果として記載しないでください。

Update both language guides together. Verify behavior, defaults and limits against
source and tests. Preparing manual fixtures or passing automated tests does not
establish that a real-terminal or external-environment check was performed.

リポジトリのルートで実行します。Run from the repository root:

```powershell
python tools/sync_readme.py
python tools/sync_doc_index.py
python tools/check_docs.py
python -m unittest discover -s tests -p test_docs.py -v
```

検査は書式、日英の構造・コード例・数値、リンクと見出し、ソース参照を確認します。
文章の意味や操作の実行結果を保証する検査ではありません。

Checks cover formatting, paired structure/examples/numbers, links and headings,
source references. They do not prove semantic
translation equivalence or the outcome of a real operation.
