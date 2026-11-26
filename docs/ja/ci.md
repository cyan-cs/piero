[English](../en/ci.md) | [日本語](../ja/ci.md)

# GitHub Actionsの検証

## 対象

`.github/workflows/validation.yml`はpush、PR、手動実行を検証します。
認証を使ったリポジトリへの書き込みは行わず、権限は`contents: read`だけです。
パッケージやリリースは公開しません。CIが成功しても、AppContainer、ファイル操作、
実際の端末での安全性を確かめたことにはなりません。

ソースを公開する前に、隠しファイルも含めて、実際に選んだファイルを確認してください。個人用のエージェントや
クラウドの設定、認証情報、環境ファイル、生成物、生の証跡は除外します。ignoreの設定をしても、すでに
追跡されているファイルは除外されません。Git履歴は別途確認してください。作業ツリーがきれいでも、秘密情報がない証明にはなりません。
シークレットの検査は、確度の高いパターンと名前を調べるもので、完全な監査ではありません。

## Actionの固定とcheckout

公式のリリースページで、各バージョンが次の完全なコミットを指していることを確認しました。
メジャーバージョンは維持し、checkoutには、あとで入った認証情報の後始末の修正を含めます。

| Action | バージョン | コミット |
| --- | --- | --- |
| actions/checkout | v4.3.1 | `34e114876b0b11c390a56381ad16ebd13914f8d5` |
| actions/setup-python | v5.6.0 | `a26af69be951a213d495a4c3e4e4022e16d87065` |
| actions/upload-artifact | v4.6.2 | `ea165f8d65b6e75b540449e92b4886f43607fa02` |
| actions/cache (restore/save) | v5.0.3 | `cdf6c1fa76f9f475f3d7449005a359c84ca0f306` |
| Mozilla-Actions/sccache-action | v0.0.11 | `fc920bf0ec8de6ee65d409111f7ec508035751ba` |

根拠：[checkoutのリリース](https://github.com/actions/checkout/releases/tag/v4.3.1)、
[Pythonのリリース](https://github.com/actions/setup-python/releases/tag/v5.6.0)、
[artifactのリリース](https://github.com/actions/upload-artifact/releases/tag/v4.6.2)。
キャッシュの根拠：[cacheのリリース](https://github.com/actions/cache/releases/tag/v5.0.3)、
[sccache actionのリリース](https://github.com/Mozilla-Actions/sccache-action/releases/tag/v0.0.11)。
すべてのcheckoutで`persist-credentials: false`を指定します。資料・履歴の検査と整形のジョブだけが
`fetch-depth: 0`で全履歴を取得し、コンパイラ、Sanitizer、解析のジョブは、既定の浅いcheckoutを使います。
テストのArtifactを保存するサービスに、追加のトークン権限は要りません。PRの作成と承認は行いません。

## コミット内容の検査

`tools/ci_changes.py`は、イベントの値をシェルに埋め込まず、JSONのデータとして読みます。
比較するのは、常にcheckoutした`HEAD`のツリーです。pushでは、存在を確かめたbeforeのコミットと、
PRではイベントのbaseコミットとの共通の祖先を使うので、通常の合成されたマージのcheckoutにも対応します。
比較元がない、ゼロ、不正である、履歴が無関係である、または手動で実行した場合は、空のツリーから全体を
比べます。ツリーを特定できない、または検査に失敗したときは失敗とし、何も言わずに成功とすることはありません。
空白の検査は、きれいな作業ツリーだけでなく、最終的なツリーの差分を対象にします。
対象のパスの数を表示し、検査して差分がなかった場合と、検査していない場合を区別します。
CRLFは改行として認めますが、その前に空白があれば失敗にします。
空白検査と整形検査は、ともに`src/third_party/`を除外し、上流のファイルを保全します。
ルートの`LICENSE`はLFでチェックアウトし、埋め込まれた本文と実行時のファイルをバイト単位で一致させます。
この検査は、固定した契約のバイト列を
書き換えません。過去の証跡や第三者のファイルには空白エラーが残っている場合があります。
検査を無効にしたり、固定した証跡を黙って書き換えたりせず、失敗を調査してください。

整形にも同じ比較を使い、ファイル名はNUL区切り、コマンドは引数の配列として渡します。
source、tests、toolsの下にある、最終状態の通常のCファイルとヘッダーが対象で、`src/third_party/`と削除済みの
ファイルは除きます。全体の検査に切り替えるときは、管理している全てのCファイルとヘッダーを検査し、整形の失敗は
ジョブの失敗にします。このヘルパーは、Gitやformatterの診断に含まれるソースの行を表示しません。
詳しくはローカルで再現してください。

`tools/check_secrets.py --history`は、現在追跡している、ignoreされていないファイルの内容も検査します。
追跡されている個人設定、鍵ファイル、生成されたバイナリは拒否し、浅い履歴での履歴検査も拒否します。
履歴で見つかった箇所はオブジェクトで示し、値は出力しません。
通常の検証作業の一環としてプロジェクトの履歴を書き換えないでください。

## 実行とArtifact

ローカルactionの`setup-ci-llvm`で、圧縮済みLLVMアーカイブをジョブ間・実行間で共有します。
キーには導入・ダウンロードhelperのハッシュを使い、復元後も固定SHA-256を検証してから展開し、
ツールのバージョンも確認します。約264 MiBのアーカイブだけを保存し、4 GiBの展開先は
保存しません。検証直後に保存するため、後続テストが失敗しても次回に利用できます。
展開はジョブごとに行い、コンパイラのパスは固定してコンパイルキャッシュを再利用します。

Clang/MSVCのDebug・ReleaseとSanitizerのジョブは、`-CompilerCache`で
sccache v0.16.0のGitHub Actions backendを使います。
CMakeのcompiler launcherを設定し、Debug情報は共有PDBからオブジェクト内の`/Z7`へ変えます。
release mutationは変更のないテストドライバーも含めてソースごとにコンパイルをキャッシュし、
毎回リンクします。それ以外のコンパイラを直接呼ぶmutation helperは従来のビルド経路を使います。
sccache actionの終了処理がヒット・ミス統計を表示します。
根拠：[sccacheの使用方法](https://github.com/mozilla/sccache#usage)。

control・mutantの判定、CTest結果、fixtureは再利用せず、テストは毎回実行します。
キャッシュが空ならコンパイルし、実際の短縮時間はActionsで計測します。
長いE2Eやmutationの実行時間は残るため、キャッシュだけで全試験が短時間になるとは限りません。
ローカル検証の既定値はキャッシュなしです。`-CompilerCache`にはsccacheのインストールが必要で、
CTestから実行するmutationにも同じ設定を渡します。

pushの検証は、既定のブランチだけでなく開発ブランチでも続けます。自動実行は、同じイベントと同じrefの
古い実行だけを置き換え、PRのrefは分けます。手動実行には一意のrun識別子を使い、自動のpushや
別の手動解析から、concurrencyグループを通じてキャンセルされないようにします。

両方のコンパイラのDebug/Releaseのビルドと全CTest、DebugのASan/UBSan、整形、資料、シークレット、
任意のMSVC解析を続けます。既知の失敗を、警告や終了コードの無視に変えることはしません。
ホストされたランナーやAppContainerの動作は、実際にワークフローを実行するまで確かめられていません。

検証ヘルパーの任意の引数`-JUnit`は、生のCTestのXMLをローカルに保存します。収集処理が書き出すのは、
テストの識別子、判定、所要時間、件数、リリース保留の状態だけです。出力、失敗メッセージ、コマンド、
プロパティ、パスは書き出しません。報告がない、不正、または空であれば、収集は失敗とし、成功と誤解されないようにします。
アップロードするのは`out/ci/ci-results.json`または`out/san/ci-results.json`だけで、失敗したときの要約も
含みます。名前でコンパイラ、構成、実行の試行を分け、保存期間は7日、隠しファイルは除き、
対象がなければ失敗にします。キャンセルされたときやランナーが失われたときは、送信できないことがあります。
生の診断情報はArtifactに含めません。Actionsのログも、共有する前に非公開のまま確認してください。

## 検証とメンテナーの確認

```powershell
python -m unittest discover -s tests -p test_ci_tools.py -v
python -m unittest discover -s tests -p test_docs.py -v
python tools/check_docs.py
```

テストは、元のGitメタデータを使わない合成リポジトリを作ります。公開前にActionの許可方針、
トークンの権限、ログへのアクセス、選んだソースを確認し、最初の実行では全てのジョブを確かめてください。
ワークフローが受理されても、リリースを認めたことにはなりません。

## プロセス操作の前提条件

`lock_policy`は合成した事実でポリシーを検証します。`lock`には、さらに祖先プロセスの情報を取得できる
対話セッションが必要です。所有するヘルパーの検査で祖先情報が不明、またはSession 0の場合は
BLOCKEDと報告し、77で終了します。`process_facts`はアカウント識別子を含めず、真偽値とAPIエラーを報告します。
統合テストのスキップは、CTestが成功終了してもNOT VERIFIEDです。前提条件を満たすWindowsデスクトップで実行します。

```powershell
ctest --test-dir out/ci --output-on-failure -j 1 -R "^(process_facts|lock_policy|lock|lock_rename_e2e|lock_copy_e2e)$"
```

ミューテーショングループでは未変更実装の成功を必須とし、クラッシュ、スキップ、タイムアウトでは判定しません。
各プロセスの期限とグループ全体の予算を分けます。Gateのミュータントは、Ninjaのシェル文字列を解析せず、
専用のCMakeターゲットで生成します。

Sanitizer構成ではclang-clのリンク処理に合わせ、選択したASan runtime thunkの
アーカイブ全体とSEH interceptorをリンクします。LLVM 23.1.3では、この指定がないと
DLLのグローバル登録の解除処理が欠落し、再ロード時に同じ文字列がODR違反と
誤診断されます。通常テストとmutationの両方でODR検査を有効に保ちます。
`asan_pool_dll`はDLLを反復ロード・アンロードし、
`sanitizer_detect_dll_global`は再ロード後の実際のグローバル領域外読み出しを検出します。

CMake 3.22以降は構成時の開発シェルのPATHを保存せず、各テストの実行時に
選択したランタイムのディレクトリを先頭へ追加します。mutationツールはVS初期化後にも
この順序を復元し、MSVC同梱の別のASan DLLが選ばれることを防ぎます。
コピーした実行ファイルのfixtureにも依存するランタイムを配置し、PATHの隔離条件と
DLL検索ポリシーは維持します。

mutationの監督処理は標準ハンドルを明示的に渡すDETACHED_PROCESSを使い、
テスト終了後にも非同期で残り得る不要な`conhost.exe`を生成しません。
元の実行期限内でrootの終了とJobのaccountingを確認し、completion portの通知で
待機を解除します。タイムアウト、待機失敗、子孫の残存ではmutationを判定しません。

同一セッションではCTestを一つずつ実行します。その内部は`-j 8`で並列化でき、
競合や負荷が結果に影響する場合は4または2へ下げます。DOSドライブの利用者は
`wf_dos_drive`を共有します。mutationグループと対応する通常テストにも共通の
ドライバーロックを設け、独立した所有fixtureのグループは並列実行します。
DOSドライブ要求が変数または非ゼロの場合はドライブロックを維持します。
listing・editor・PDFデコードの時間検査は他のCTest作業と重ねず、基準値は変えません。
再試行前の失敗を保存し、中断後に再利用するのは完了が確認できた個別結果だけです。

終了コード77で正常に終了したcontrolは、理由付きのCTest SKIPとして伝播し、mutantのkillには数えません。
監督処理のエラー、不完全なreceipt、タイムアウトは失敗のままです。ConPTYの画像・ロック試験は、
AppContainer profile作成とRestart ManagerをWindows APIで直接事前確認します。コピー試験は実際の
宛先の祖先ディレクトリを読めるか確認します。既知のOS前提条件不足はエラーコードを保存してSKIPにし、
想定外のエラーは失敗にします。別名パスへの置換や製品の安全ポリシーの緩和は行いません。
バッチ実行の完全なcontrolには、復旧用子プロセスと大きなfixtureを含む通常試験の既存の時間枠を使い、
最初の失敗で終了するmutantのハング検出期限は維持します。
テーマ、画像の異常入力、fuzzy workerのcontrolにも通常試験の時間枠を使います。
正常なMSVC DebugのCIでも、mutant用の15秒を超える場合があるためです。
セッション位置のassertionは、大きな折り返し描画fixtureより先に実行します。
コンパイル失敗とcontrol失敗にはローカルの証拠パスを表示し、control失敗には
supervisorの状態、終了コード、経過時間も表示します。

Mutation定義は、意図しない未定義の算術演算、範囲外の走査、解放済みポインターを発生させずに
assertionへ到達させます。検索worker試験は列挙中の処理をイベントで同期し、偽の列挙処理もcancelを
受け入れます。専用のcancel callbackのassertionでも検証します。
検索fixtureはnonceとmanifestを持つ新しい所有領域を使い、ネイティブの後片付けも確認します。
失敗したmutantがfixtureを残した後でPIDが再利用されると、その隠しファイルをCREATE_ALWAYSで
開く処理が失敗し、検索自体を試験できなくなるためです。

ハンドル数の試験は、Windowsの対象コードページのキャッシュを基準値計測前に初期化します。
非ACPの初回検証でNls\\CodePageレジストリキーがプロセス終了まで保持されるためです。
ワーカーのハンドル数は、引き続き基準値への完全一致で確認します。ハッシュのcancelは完了通知が
来ないこととワーカーがidleになることの両方を確認し、観測時間後も処理中なら失敗にします。
状態フォルダーのfixtureは製品と同様にTokenUserを所有者として明示し、管理者のTokenOwnerにも
対応します。ロックE2Eは所有するヘルパーのcloseやterminateを要求する前に、通常のlock試験と同じ
プロセス情報の前提条件を確認します。祖先が不明なら操作を拒否したままSKIPを記録します。
E2Eの失敗したcheck識別子は自由文の証拠を含めずに出力し、ホストされたログでも判定箇所を示します。
自由文を含むCSVはローカルに保持します。

`product_tui`のタイムアウトでは、CTestの失敗出力にある最後の`PRODUCT-TUI phase=...`を
確認します。各段階の開始・結果・経過時間を出力し、checkの結果もその都度ローカルのCSVへ
書き出します。この診断は150秒の制限を変更せず、中断したシナリオをPASSにはしません。

`keyboard_profiles`は同じVim/Nanoの操作をテキストfixtureで検証し、画像デコーダーや
Shell関連付けの前提条件を必要としません。ファイル一覧にも`NORMAL`があるため、
エディターへの移行は固有のステータス区切りまで待ちます。
完全な`product_tui`と`ui_audit`も引き続き必要です。
