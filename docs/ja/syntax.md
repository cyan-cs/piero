[English](../en/syntax.md) | [日本語](../ja/syntax.md)

# シンタックスハイライト

pieroは、プレビューペインと内蔵エディタの文字を、Tree-sitterの文法(Grammar)で色分けします。Grammarは`piero.exe`の一部ではありません。それぞれが独立したDLLで、プログラムの隣に置いたものだけが使われ、ファイルは対応するGrammarが入っているときだけ色分けされます。Grammarがなければ何も変わりません。文字は色なしで表示され、エディタはこれまでの簡易的な色分けのままです。

## 色分けされるもの

| 言語 | ファイル |
| --- | --- |
| JSON | `.json`, `.geojson`, `.webmanifest` |
| JSONC | `.jsonc`, `.code-workspace`, `tsconfig.json`, `jsconfig.json`, `settings.json`, `launch.json`, `tasks.json`, `extensions.json` |
| YAML | `.yaml`, `.yml` |
| TOML | `.toml`, `Cargo.lock`, `Pipfile` |
| INIとCFG | `.ini`, `.cfg`, `.inf`, `.desktop`, `.url`, `.editorconfig`, `.gitconfig`, `.gitmodules` |
| XML | `.xml`, `.xsd`, `.xsl`, `.xslt`, `.svg`, `.xaml`, `.csproj`, `.vcxproj`, `.props`, `.targets`, `.plist`, `.config`, `.manifest`, `.resx`, `.nuspec`, `.rss`, `.atom`, `.wsdl` |
| Markdown | `.md`, `.markdown`(プレビューペインはMarkdownを整形して描くため、ソースを色分けするのはエディタだけです) |
| PowerShell | `.ps1`, `.psm1`, `.psd1` |
| Shell | `.sh`, `.bash`, `.bashrc`, `.bash_profile`, `.bash_aliases`, `.profile`, `PKGBUILD` |
| dotenv | `.env`, `.env.local`, `.env.example`, `.env.development`, `.env.production`, `.env.test` |

ファイル名の全体が拡張子より優先されるので、`tsconfig.json`はJSONCになり、ほかの`.json`はJSONになります。INIはGrammarが読む方言(`[sections]`、`name = value`、コメント)で、値は色なしのままです。dotenvにはGrammarのDLLがありません。一つの形の行には構文解析器が要らないため、`piero.exe`の中の字句解析が色分けします。

種類ごとの色(キー、文字列、数値、真偽値、null、コメント、見出し、リンクなど)はテーマから決まり、すべてのプリセットとカスタムテーマで使えます。テーマを変えると色が塗り直されるだけで、構文解析はやり直しません。

## 導入

同じビルドの任意のファイルを`piero.exe`の隣にコピーします。現在のパッケージ作成スクリプトは
文法パックを同梱しません。[パッケージ](packaging.md)を参照してください。
自分のアカウントで書き込めるフォルダーを使います。

```text
piero.exe
syntax\piero-syntax.dll
syntax\grammars\json\parser.dll
syntax\grammars\json\highlights.scm
syntax\grammars\json\manifest.toml
syntax\grammars\toml\...
```

| パッケージ | 中身 |
| --- | --- |
| Core | 任意の`syntax`フォルダーを含まない基本パッケージ。文法による色分けはありません。 |
| Config pack | `syntax\piero-syntax.dll`と、Grammarの`json`、`jsonc`、`yaml`、`toml`、`ini`、`xml`です。 |
| Extended pack | `syntax\piero-syntax.dll`と、Grammarの`markdown`、`markdown_inline`、`powershell`、`bash`です。 |
| 単体のGrammar | `syntax\piero-syntax.dll`と、`syntax\grammars\`の中のフォルダー一つです。 |

`piero-syntax.dll`はどのパックにも入っているので、パック一つだけで完結します。二つ目のパックを入れると、同じファイルで置き換わります。`markdown_inline`はMarkdownの二つ目のGrammar(強調、コードスパン、リンク)で、なければMarkdownはブロック(見出し、フェンス、リスト)だけが色分けされ、行内の記法は色なしのままです。Grammarを外すには、そのフォルダーを削除します。読み込まれているGrammarにpieroは手を加えず、削除したものは次の起動で見つからないだけです。

実行ファイル、構文色分け用DLL、文法ファイルは同じビルドのものを使ってください。
同じバージョンでも、別のコンパイラでビルドするとハッシュが異なる場合があります。
更新時は、対応する実行ファイルと`syntax`フォルダー全体を一緒に置き換えてください。

## 動作

- 起動時には何も読み込みません。一覧表示、検索、ファジー検索ではGrammarを読み込みません。Grammarが入っている言語のファイルを初めて表示したときに、専用のスレッドが`piero-syntax.dll`を読み込み、続いてそのGrammarだけを読み込みます。見ない言語のGrammarは、入っていても読み込まれません。
- 文字はすぐに色なしで表示され、専用のスレッドが答えると色が付きます。別のファイルへ移ると、前のファイルの色は捨てられます。
- エディタでは、入力に色が追従します。文字が変わったとき(カーソルを動かしただけでは送りません)に専用のスレッドへ渡し、新しい色が届くまでは古い色が編集の分だけずれて残り、構文解析は前の木を再利用します。元に戻す・やり直すも、ほかの変更と同じ文字の変更です。色が、文字、文字コード、改行、保存される内容を変えることはありません。
- 読み込んだGrammarはpieroが終了するまで保持され、終了時にまとめて解放されます。専用のスレッドがまだ使っているかもしれない間は、解放しません。
- Grammarが使えない場合(ない、拒否された、壊れている、遅すぎる)は、文字は色なしで表示され、編集には影響しません。

## 上限

2 MiBを超える文字、64 KiBを超える行、400 msを超える構文解析、200 msを超えるクエリ、1,000,000を超える色の区間のいずれかに当たると、その文字は色分けされず、これまでどおり表示・編集できます。プレビューペインは読み取ったテキストを色分けします。既定は256 KiBで、`[preview] text_max_kib`により最大1024 KiBまで変更できます。

ファイル名の判定対象は、現在は64文字未満のASCII名だけです。日本語を含む名前などは対象外で、
プレビューは色なし、エディターは任意の文法ではなく内蔵の色分けを使います。

## 安全性

GrammarのDLLは、pieroの権限で動くネイティブコードです。次の手順は、間違った、または一部だけ置き換えられた導入を失敗させますが、悪意のあるDLLを安全にするものではありません。

- このビルドに固定したGrammarだけを読み込めます。`piero-syntax.dll`は、公式の`parser.dll`、`highlights.scm`、`manifest.toml`それぞれのSHA-256を持ち、`piero.exe`は`piero-syntax.dll`のSHA-256を持ちます。一致しないファイルは読み込まれず、その拒否は次の起動まで保たれます。
- `manifest.toml`はGrammarの説明であり、何も追加しません。DLLの中の表と一致しなければならず、厳密に(大きさ、長さ、重複キー、未知のキー、ファイル名)読まれます。
- フォルダーは、実行中の`piero.exe`のフォルダーです。カレントディレクトリとPATHは探しません。途中にシンボリックリンク、ジャンクション、マウントポイントがあるパス、ネットワークのパス、ローカルでないドライブは拒否されます。ファイルを検査して読み込む間は開いたまま保持し、その間に誰も変更できないようにします。
- DLLの検索はSystem32とDLLのフォルダーに限られ、Grammarが取り込むのは`kernel32`だけです。
- GrammarとDLLのABIの版は、読み込んだ後に確認します。これはpieroがすでに実行すると決めたファイルの整合性の確認であり、保護ではありません。
- 書き込める場所へのポータブルな導入では、そこへ書き込める人は`piero.exe`も置き換えられます。その場合の確認が守るのは、一部だけの変更(置かれたGrammar、別の版のパック)に対してだけです。コード署名はまだありません。
- このビルドで固定した文法だけを受け付けます。独自の文法の追加には対応しません。

## 未対応

C/C++、Pythonなど上の表にない言語のGrammar、マニフェストが指定するGrammarの読み込み、Markdownのコードフェンスの中の別の言語、そのほかのプレビュー(CSV、画像)の色分け、pieroの実行中にGrammarを解放すること。
