[English](../en/packaging.md) | [日本語](../ja/packaging.md)

# Windowsパッケージ

ビルドとパッケージ作成ではpiero.exeを生成します。パッケージには、ユーザーPATH用のヘルパー、`languages/`、MITのLICENSE、
変更していない第三者のLICENSESフォルダーが入ります。作成処理は、静的CRTを使い、Sanitizerを含まない
Releaseビルドを求め、通常のDLLインポートと遅延ロードのDLLインポートを調べて、確認できていないシステム外の依存を拒否します。
起動させるために、任意のDLLをコピーしないでください。

x64 Developer PowerShellで、outの下に新しいディレクトリを指定します。

```powershell
powershell -NoProfile -File tools/package_windows.ps1 -Version 0.0.3-dev -BuildDirectory out/package-build-new -OutputDirectory out/package-new
python tools/check_package.py out/package-new/payload --executable piero.exe --installer-helper
```

`piero-windows-x64.zip`と、インストーラー用ペイロードの正確なハッシュとバージョンの一覧を
生成します。例のバージョンは開発中の候補で、公開された安定版ではありません。
実機での確認が済んだあとで、メンテナーが正式なバージョンを選びます。
ビルドのバージョンは更新確認と同じ範囲にそろえ、ASCIIで最大63文字、コア番号は32ビットのSemVerを使います。
ZIPは、新しいディレクトリに展開すれば、
インストールせずに起動できます。

setupもコンパイルする場合は、x64compatibleに対応したInno SetupをPATHに追加してください。

```powershell
powershell -NoProfile -File tools/package_windows.ps1 -Version 0.0.3-dev -BuildDirectory out/installer-build-new -OutputDirectory out/installer-new -Installer
```

出力は`piero-Setup-x64.exe`です。Windows 11(build 22000以降)を要求します。管理者権限なしで、ユーザーのProgramsディレクトリにインストールし、
アンインストールの登録とスタートメニューを作ります。ユーザーPATHへの追加は、既定では選ばれていません。
追加したあとは、端末を再起動してください。ヘルパーは、既存のレジストリ文字列の型を保ち、同等の項目を
重複して追加せず、自分が追加した内容を正確に記録します。アンインストールでは、追加したときの状態と一致する場合
だけ元に戻します。ほかのプログラムがPATHを変更していたときは何も変えず、手動で確認するよう案内します。
マシンのPATHの変更や、個人の状態の再帰的な削除はしません。
setupは、すでにファイルがあるインストール先や、登録済みのインストールを拒否するので、古いアプリを先に削除してください。
実行中のアプリを自動で終了することはありません。レジストリの比較は、関係のないPATHへの書き込みを
原子的にロックするものではないので、setupを配布する前に、捨ててよいVMでインストールと競合を確かめてください。

作成処理は、ZIPとインストーラーのペイロードの、バイト列とバージョンを照合します。
コンパイルと、実際のインストール・削除は、別の検査です。コード署名の鍵は未設定で、別に署名・検証しない
ローカルの出力は署名されていません。SmartScreenが警告しても、Windowsの保護は解除しないでください。
スナップショット付きの、捨ててよいVMで、初回の起動、PATHの変更、更新、削除を確かめてください。
公開状況は公式Releasesで確認してください。ローカルでパッケージを作成してもリリースは公開されません。

構文色分けは任意です。現在のZIP作成処理とInno Setup定義は、ビルド先の`syntax`を同梱しません。
文法を配布する場合は`piero.exe`と同じビルドのフォルダーを別途用意し、ライセンス表示を保持して
各ファイルを確認してください。ConfigとExtendedのパック区分は配置の説明であり、
`package_windows.ps1`が追加のアーカイブを生成するわけではありません。
配置とハッシュ検査は[シンタックスハイライト](syntax.md)を参照してください。
