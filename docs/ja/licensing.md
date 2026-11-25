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
配布用にはBSDの通知を残しています。同梱しているxzのツリーには、別の条件のツールやビルド用ファイルも
含まれるので、ツリー全体を0BSDと書かないでください。ソース一式を公開するときは、
上流の通知をすべて残す必要があります。`cmake/product.cmake`は、チェックアウトの`LICENSES/`を
製品の横へコピーします。

## まだ確認が必要なこと

配布する前に、コンパイルする各ソースの個別の通知と出どころを確認してください。部品の一覧は
法務監査の代わりにはなりません。libarchiveのまとめた通知が、選んだファイルの全体を含んでいることを確認し、
更新するときも上流の通知を残してください。ASan/compiler-rtは開発専用なので、通常のパッケージに入れないでください。

[MITライセンス本文](https://opensource.org/license/mit)

## Tree-sitterと文法

Tree-sitterの実行環境(MIT)と、任意の`syntax`フォルダーのためにpieroがビルドする文法(json、jsonc、yaml、toml、ini、
xml、markdown、powershell、bash。iniだけがApache-2.0で、ほかはMIT)は、ソースツリーに取り込んであり、通知は
`LICENSES/tree-sitter-0.27.2.txt`と`LICENSES/tree-sitter-grammars.txt`にあります。
`grammar-pack`のクエリはpiero用に書いたものです。文法のバージョンを更新するときは、その通知を確認し直してください。
