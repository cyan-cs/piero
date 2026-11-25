[English](getting-started.md) | [日本語](../ja/getting-started.md)

# Getting started

## Install and launch

The target platform is Windows 11 x64. Windows 10 is untested; the installer
requires Windows 11 (build 22000 or later).
Check [GitHub Releases](https://github.com/cyan-cs/piero/releases) for available packages.
Extract a portable ZIP in full to a new folder, or follow setup if an installer is available.
Keep `languages/`, `LICENSE` and `LICENSES/` beside `piero.exe`.
Packages use a static CRT and do not need a separate Visual C++ runtime.
Git is needed only for Git integration.

Open a terminal in the extracted folder and run:

```powershell
.\piero.exe
```

To start in another folder, pass its path: `piero.exe C:\Work`.
Without a published package, use [build from source](development.md#build-from-source).

## First run

Start with disposable local files. Setup asks for your language, keyboard profile
and theme. `piero --setup` repeats setup; `piero --language ja` selects Japanese
for one run. `?` shows the active keys and `,` opens Settings.

With Standard keys, use arrows to navigate, Space to select, `e` to edit, and
`q` to quit the file manager. To copy, press `y`, go to the destination and press `p`.
Other profiles differ; see [keybindings](keybindings.md).
Before moving, deleting or overwriting important files, read [safety](safety.md).
Overwrites cannot be undone; Undo is limited to selected operations in the current run.

A separate prompt asks whether release checks may contact GitHub; the default is off.
Checks fetch release information only. Downloads and installation remain manual.
See [updates](updates.md).

## Settings and saved data

Settings normally use `%APPDATA%\piero\config.ini`. `--config <path>` requires
an existing valid file. Configuration is stored in your user profile even with
the portable package. See [configuration](configuration.md) for settings, drafts
and optional session restoration.

Local records, drafts and working files normally use `%LOCALAPPDATA%\piero`.
If only the older `wfile` folder exists, it stays in use; when both exist, piero
uses `piero` and checks the older folder for unresolved records. No automatic
migration or deletion takes place. Preserve records, backups and originals before
removing state. Do not delete the state folder to dismiss a recovery warning.
See [safety](safety.md) and [troubleshooting](troubleshooting.md).

## Preview and troubleshooting

Image/PDF preview is enabled by default and needs working AppContainer isolation.
Settings > Preview > Pictures turns it off. Sixel requires compatible terminal
replies and pixel dimensions; text-cell display is the fallback.
Use [troubleshooting](troubleshooting.md) for preview failures, save errors and recovery notices.
Package creation and the remaining install/uninstall checks are in [packaging](packaging.md).

## Command-line helpers

`--help` and `--version` print English text. `--print-last-dir <new-file>`
writes BOM UTF-8 on normal exit after successful navigation, using a new file;
it refuses an existing name. Inside an archive, it records the containing folder.
`--reset-terminal` emits a terminal reset. Recovery commands and the unavailable
`--purge-state` option are described in [safety](safety.md).
