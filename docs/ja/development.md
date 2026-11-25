[English](../en/development.md) | [日本語](../ja/development.md)

# 開発

## ソースからのビルド

C++ツールとWindows SDKを用意し、x64 Visual Studio Developer PowerShellを使います。
CMake 3.20+、Ninja、Python 3.10+、clang-clまたはMSVCが必要です。
開発の対象はWindows 11 x64です。CIでは`tools/setup_ci_llvm.ps1`でコンパイラを選びますが、
ローカルでは特定のパッチバージョンを指定していません。
一部のテストがMAX_PATHを使うため、ビルド先の絶対パスは90文字未満にしてください。

リポジトリのルートで、clang-clを使ってビルドします。

```powershell
cmake -S . -B out/dev -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=Debug
cmake --build out/dev
ctest --test-dir out/dev --output-on-failure -L non-destructive
.\out\dev\piero.exe .
```

MSVCではビルド先を分け、`-DCMAKE_C_COMPILER=cl`を指定します。
製品だけをビルドする場合は`-DBUILD_TESTING=OFF`を指定してください。
ファイル操作のテストは名前付きDOSドライブのミューテックスを共有するため、CTestを複数同時に実行しないでください。
後述のスクリプトでもビルドと検証ができます。制限された環境での実行を考慮し、PATHより先に
Visual Studio付属のNinjaを探します。
PowerShellスクリプトの実行は所属組織のポリシーに従ってください。実行できない場合は、
開発者シェルで上記のCMakeコマンドを直接使います。

## ビルドと検証

clang-cl、MSVC、Sanitizerには、それぞれ別の短いパスのビルドディレクトリを使います。
本体はISO C17です。コーディング規約と貢献ガイドも読んでください。第三者ライブラリの
警告の範囲と通知は、そのまま維持します。通常のビルドで、製品の依存を新しくダウンロードすることはありません。

```powershell
powershell -NoProfile -File tools/validate_publication.ps1 -Toolchain clang -Configuration Debug -SafeOnly -BuildDirectory out/dev-clang
powershell -NoProfile -File tools/validate_publication.ps1 -Toolchain msvc -Configuration Release -SafeOnly -BuildDirectory out/dev-msvc
powershell -NoProfile -File tools/validate_publication.ps1 -Toolchain clang -Configuration Debug -Sanitize -SafeOnly -BuildDirectory out/dev-san
python tools/sync_readme.py
python tools/sync_doc_index.py
python tools/check_docs.py
```

一部のテストだけでは、実際のファイル操作を確かめたことになりません。ファイル操作全体、中断、
ボリュームをまたぐテストには、手動検証で示している、所有を確認した捨ててよい環境が必要です。
確認を通すために、操作の禁止を緩めたり、失敗をスキップに変えたりしないでください。
MSVCの解析は参考情報としてclだけに限り、clang-clではその設定を拒否します。

Sanitizerは、選んだコンパイラに合ったランタイムを使います。保護された、Debug以外の親プロセスは
私用アセンブリで起動しますが、コピーしたデコーダーの配置での確認はまだ終わっていません。
通常のパッケージにSanitizerのランタイムを入れないでください。PDFの隔離、実際の端末、ストレージの
永続性には、それぞれ別の証拠が必要です。

日英の対応を保ってください。ルートの英語のREADMEが原本で、編集したら同期ツールを使います。
生成される目次には現行のガイドを並べます。ガイドの更新とテストの手順は[ドキュメントの入口](../README.md)を参照してください。
新しい検証結果は、再現に必要なリビジョン、ビルド構成、環境とともに、ignore対象のビルド先に保存してください。
更新確認のテストは、APIとレジストリを差し替えて行い、通信やホストのPATHの変更はしません。
パッケージの作成と公開は、別の作業です。

## ベンチマーク

テストを有効にしたビルドの`bench_core`は、決まった入力をウォームアップしてから測り、
新しいディレクトリに`summary.json`と`samples.csv`を保存します。すでにある出力先は拒否します。
P50/P95/P99、出力バイト数、プロセスの資源の数を記録します。CIに性能のしきい値は設けていません。

```powershell
cmake --build out/dev-msvc --target bench_core
out/dev-msvc/bench_core.exe --output out/bench-first --samples 101
python tests/test_benchmark.py out/dev-msvc/bench_core.exe
```

測るのは、CPUでのフレーム生成、入力からフレーム生成まで、VTの生成、縮小、Sixel、
ディレクトリモデルの生成とソートです。端末への描画や、実際のコンソールへの書き込み回数は含みません。
`--enumerate`に、所有を確認済みの既存のテスト用ディレクトリを渡すと、読み取り専用のWin32列挙も測ります。
テストデータを作る時間は含みません。決まった入力には、ユーザーの翻訳による上書きを使いません。
プロセスのメモリが増えたというだけでは、リークの証明にはなりません。同じビルド、入力、環境で比べてください。

## デコーダーとSanitizerの診断

AppContainerのプロファイル作成、プロセス起動、デコードの失敗を区別してください。
[probe_appcontainer.c](../../tools/probe_appcontainer.c)はプロファイル作成、
[probe_decoder_start.c](../../tools/probe_decoder_start.c)は製品のデコーダー起動を調べます。
SIDを導出できてもプロファイル作成の成功は証明できず、HRESULTだけでは不足する資源を特定できません。
通常のユーザーセッションと失敗する環境を比較し、ユーザー識別情報やプロファイルの生のトレースは公開しないでください。

Debug以外のビルドは`/DEPENDENTLOADFLAG:0x800`を使うため、compiler-rtをPATHに追加しても
保護された実行ファイルの静的インポートは解決しません。Sanitizer付きの親プロセスは、
選択したランタイムの私用アセンブリ`piero.asan.runtime`を使います。これだけでは別途配置する
デコーダーの動作確認にはなりません。ASanをSystem32に配置したり、通常の配布物に同梱したりしないでください。
親プロセスとデコーダーの起動結果は、コンパイラのバージョンと使用したランタイムを含めて別々に記録してください。

## セッションの保全

`src/session/wf_session_store.c`は、保存ロックを取得してから既存データを検証します。
復元せず保存だけを行う起動でも、破損、読み取り不能、未対応形式のデータには書き込みません。
この禁止状態を解除するのは、両方のスロットを正常に空にした場合だけです。
もう一方に正常な対応形式のスロットがあれば、通常のスロット読み取りで書きかけのデータを避けて復元できます。

`tests/test_main_session.c`は、捨ててよいセッションファイルと偽のナビゲーションを使い、
`src/main_session.c`の`wf_main_session_start`から定期保存、終了までを実行します。
復元の有無、ヘッダーの読み取り失敗、未対応バージョンでのバイト単位の保全と、明示的な消去後の保存を確認します。
`tests/test_session.c`では、消去に失敗しても、その後の上書きを許さないことも確認します。

`tests/test_session_alloc.c`は、符号化・復号の各確保を失敗させ、完全なスナップショットまたは
`WF_E_NOMEM`が返ること、失敗時の出力が空で、部分データが解放されることを確認します。
重複抑止キャッシュの更新に失敗したとき、古いスナップショットを忘れることも確認します。
