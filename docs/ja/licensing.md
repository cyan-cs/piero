[English](../en/licensing.md) | [日本語](../ja/licensing.md)

# ライセンス確認

## 本体のライセンス

本体として権利を持つコードと資料には、
ルートの[LICENSE](../../LICENSE)を適用し、「2026 piero contributors」と表記します。
対象のコードを再配布するときは、著作権と許諾の表記を残してください。
本体への貢献にも同じMIT条件を適用します。提供する権利のあるものだけを提出してください。
これは、第三者のコードのライセンスを変えるものでも、外部からコピーしたものの権利を確認したものでもありません。

## pieroでライセンスを見る

設定の「情報」には、プロジェクトの短い紹介と、独立したMIT Licenseの項目があります。
紹介の最後にはスポンサーの案内と、メンテナーのXアカウント`https://x.com/dev_cyan`を表示します。
特典や開発上の優遇は約束しません。リポジトリのリンクは
`https://github.com/cyan-cs/piero`です。

MITの項目と、「ライセンス」にあるpieroの項目には、ルートの`LICENSE`から直接生成した
英語の正式な本文を、著作権表示も含めて全文表示します。翻訳や要約には置き換えません。CMakeはそのファイルの
変更を検知して本文を埋め込むので、実行時に外部ファイルがなくても読めます。
Rightで本文に移り、Up/DownまたはPage Up/Page Downでスクロールし、Home/Endで先頭・末尾へ、
Leftで項目の一覧へ戻ります。Escで設定から戻ります。

第三者の項目は、`LICENSES/`の通知を表示します。配布物には本体のライセンスと
これらの通知を含めてください。MITの本文を埋め込んであっても、通知を省いてよいことにはなりません。

## 同梱しているソースと通知

| コンポーネント | バージョン | ソースの通知 | 配布用の通知 |
| --- | --- | --- | --- |
| libarchive | 3.8.10 | `src/third_party/libarchive-3.8.10/COPYING` | `LICENSES/libarchive-3.8.10.txt` |
| zlib | 1.3.1 | `src/third_party/zlib-1.3.1/LICENSE` | `LICENSES/zlib-1.3.1.txt` |
| liblzma | 5.8.1 | `src/third_party/xz-5.8.1/COPYING.0BSD` | `LICENSES/xz-liblzma-5.8.1.txt` |
| zstd | 1.5.7 | `src/third_party/zstd-1.5.7/LICENSE` | `LICENSES/zstd-1.5.7.txt` |

libarchiveには、ファイルごとのBSDやパブリックドメインの表記と、全体をまとめたCOPYINGがあります。
zlibはzlibライセンス、liblzmaライブラリは0BSD、zstdはBSDとGPLから選べ、
配布用にはBSDの通知を残しています。上流のxz配布一式には、別の条件のツールやビルド用ファイルも
含まれるので、ツリー全体を0BSDと書かないでください。ソース一式を公開するときは、
上流の通知をすべて残す必要があります。`cmake/product.cmake`は、チェックアウトの`LICENSES/`を
製品の横へコピーします。

## 通知の網羅性を確認する

配布用の通知には、コンパイルされる外部ソースとヘッダーの個別の著作権・ライセンス表記も収録しています。
libarchive内部の借用コードも対象です。Tree-sitterの通知には同梱ICU/Unicodeヘッダーのライセンス全文を含めています。
ZstandardはBSDライセンスを選択しています。

ビルド後に次のコマンドで確認できます。

```powershell
python tools/audit_licenses.py out/cr --output out/license-review.json --check-notices
```

CTestの`license_notices`も同じ確認を実行します。C翻訳単位とNinjaが記録した実際の依存ヘッダーを調べ、
上流のライセンス本文を照合し、未登録の外部コンポーネントを拒否します。通知だけを変更した場合も、
既存ビルドの製品の横へコピーし直します。ZIPとインストーラーのペイロードには通知をそのまま含めます。
ASan/compiler-rtは開発専用なので、通常のパッケージに入れないでください。

これは通知の網羅性の確認であり、権利の帰属やソースの出どころ、上流のすべての利用条件を保証するものではありません。
依存関係の更新時には条件や来歴を確認してください。新しいコメントを通知へコピーするだけでは法務確認になりません。

## Unicodeデータの指摘を解決（2026-11-27）

元のlibarchive 3.8.10の`archive_string_composition.h`はUnicodeData 6.0.0から生成されていましたが、
表記はlibarchiveの通知だけでした。上流でも[issue #3817](https://github.com/libarchive/libarchive/issues/3817)で
Unicodeの通知漏れが指摘され、[PR #3848](https://github.com/libarchive/libarchive/pull/3848)でデータ更新と
ライセンス追加により解決しています。今回、ヘッダーの両方の変更を取り込み、適用結果のGitハッシュが
上流パッチと一致することを確認しました。その後、末尾の余分な空行だけを除去しています。
現在のテーブルはUnicodeData 18.0.0由来で、Unicode License v3を含みます。
バイナリー配布用の`LICENSES/libarchive-3.8.10.txt`にも通知全文を収録しています。

2026-11-27の取り込みの来歴は、外部ソースの`PIERO-LICENSING.md`に記録しています。
回帰テスト`archive_unicode`は実際にリンクしたライブラリのNFC/NFD変換を使い、6.0以降の文字、日本語、
結合文字、ハングル、合成除外を確認します。通知チェックでも、Unicode通知のない旧データへの差し戻しを検出します。
これで既知の通知漏れは解決しています。ただし、上流自体にない条件を自動検査で発見できるわけではありません。

[MITライセンス本文](https://opensource.org/license/mit)

## データとソース配布の追加確認

`src/text/wf_width.c`と`src/text/wf_width_ambiguous.h`のUnicode 15.0.0文字幅範囲は、
Pythonの`unicodedata`経由のデータも含む、文字データを加工した表現です。Python処理系のコードは含みません。
`LICENSES/unicode-data.txt`にデータの通知、許諾本文、出典を保持し、生成スクリプトもその通知への参照を出力します。

ソースツリーにはlibarchiveのGNU Autoconf/Libtool補助ファイルも残っています。
各ファイルのGPL通知と例外は保持し、その外部ソースのディレクトリーへGPLの全文を追加しています。
これらはWindows製品へ組み込まず、パッケージにも含めません。ソース全体をMIT、libarchive全体をBSDと
説明しないでください。ルートのMITはプロジェクト所有の部分に適用されます。

ポータブル版は静的CRTを使い、確認済みWindowsシステムDLLだけをインポートします。
Python、LLVM、CMake、Ninjaはビルド用で、プログラムを同梱しません。
インストーラーにはInno Setupのエンジンも含まれるため、
[Inno Setupのライセンス](https://jrsoftware.org/files/is/license.txt)に従い、既存の著作権表記とWebアドレスを保持します。
この環境にはインストーラーのコンパイラーがなく、実際のインストーラーバイナリーは未確認です。

## 文法ソース

Tree-sitterの実行環境(MIT)と、任意の`syntax`フォルダーのためにpieroがビルドする文法(json、jsonc、yaml、toml、ini、
xml、markdown、powershell、bash。iniだけがApache-2.0で、ほかはMIT)は、ソースツリーに取り込んであり、通知は
`LICENSES/tree-sitter-0.27.2.txt`と`LICENSES/tree-sitter-grammars.txt`にあります。
`grammar-pack`のクエリはpiero用に書いたものです。文法のバージョンを更新するときは、その通知を確認し直してください。
