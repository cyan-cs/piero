# piero — Terminal file manager for Windows

![Windows 11 x64](https://img.shields.io/badge/Windows_11-x64-0078D4?style=flat-square)
![C17](https://img.shields.io/badge/C-17-A8B9CC?style=flat-square)
![MIT License](https://img.shields.io/badge/License-MIT-34D399?style=flat-square)
![Development Preview](https://img.shields.io/badge/Status-Development_Preview-FBBF24?style=flat-square)

**English** | [日本語](docs/ja/README.md)

piero is a keyboard-driven file manager with a built-in text editor for Windows.
Browse folders side by side, preview files and make quick edits in your terminal.

> **Development preview:** Back up important files independently before use.
> Overwriting a file cannot be undone in piero. Real-terminal, installer and
> power-loss validation is still incomplete. Read the [safety guide](docs/en/safety.md).

## Sponsors

Sponsor acknowledgments are kept in [SPONSORS.md](SPONSORS.md).
Corporate logos may appear here with permission under the [sponsorship policy](docs/en/sponsorship.md).

## Features

- **File management:** Copy, move, rename, bulk rename and send local files to the Recycle Bin. Undo is available for selected operations.
- **Navigation:** Side-by-side folder columns, tabs, saved places, filters and recursive name search.
- **Preview:** Text, Markdown, CSV/TSV, images and PDF. Browse supported archives; extract ZIP entries.
- **Editing:** Edit text with encoding and line-ending preservation, search and replace, syntax colouring and a live Markdown preview.
- **Git:** See file status and compare HEAD with the editor buffer, including unsaved changes. Git is optional.
- **Your environment:** English and Japanese UI, Standard/Nano/Vim/Custom keymaps and configurable themes. WSL file operations and read-only SFTP browsing are also available.

See [features and limitations](docs/en/features.md) for supported formats and differences between local, WSL and SFTP locations.

## Installation

Target platform: **Windows 11 x64**. Check [GitHub Releases](https://github.com/cyan-cs/piero/releases) for available preview packages.

- **Portable ZIP:** Extract the whole package to a new folder and run `piero.exe` from a terminal. Keep `languages/`, `LICENSE` and `LICENSES/` beside it.
- **Installer, when available:** Follow setup. It installs for your account without administrator rights; adding piero to your user PATH is optional.

Packages use a static C runtime; a separate Visual C++ runtime installation is not required.
If no package is available, follow [build from source](docs/en/development.md#build-from-source).
See [getting started](docs/en/getting-started.md) for first-run setup and [packaging](docs/en/packaging.md) for package qualification limits.

## Basic usage

Start `piero.exe`, or pass a folder: `piero.exe C:\Work`.
Choose your language, keyboard profile and theme at first launch.

With the **Standard** profile:

| Key | Action |
| --- | --- |
| Arrow keys, `h` / `l` | Move through files and folders |
| Space | Select an item |
| `y`, then `p` in the destination | Copy selected items, or the item under the cursor |
| `e` | Open the built-in editor |
| `?` | Show the active key bindings |
| `,` | Open Settings |
| `q` | Quit the file manager |

Other profiles use different keys. See [key bindings](docs/en/keybindings.md).

## Support and participation

Bug reports, feature suggestions, translations and code contributions are welcome:
[GitHub Issues](https://github.com/cyan-cs/piero/issues) · [Contributing](CONTRIBUTING.md).
Report security problems through the private channel described in [SECURITY.md](SECURITY.md).

For financial support or corporate logo placement, contact [@dev_cyan on X](https://x.com/dev_cyan).
See the [sponsorship policy](docs/en/sponsorship.md) for consent, placement and duration.
Individual sponsors receive equal recognition regardless of contribution amount.

## Documentation

[All guides in English and Japanese](docs/en/index.md)

| Guide | Contents |
| --- | --- |
| [Getting started](docs/en/getting-started.md) | Installation and first use |
| [Features](docs/en/features.md) | Availability, formats and limitations |
| [Key bindings](docs/en/keybindings.md) | Profiles and shortcuts |
| [Configuration](docs/en/configuration.md) | Settings, drafts and session restoration |
| [Safety](docs/en/safety.md) | Overwrite, deletion, backups and recovery |
| [Troubleshooting](docs/en/troubleshooting.md) | Errors and recovery guidance |
| [Updates](docs/en/updates.md) | Optional release checks and manual updates |

## Requirements and limitations

- Windows 10 is untested; the installer requires Windows 11 (build 22000 or later).
- Image/PDF preview requires working AppContainer isolation. Image formats and Sixel display depend on installed codecs and terminal support.
- Git integration requires `git.exe` on Windows, or `git` inside the target WSL distribution. Normal file management and editing do not require Git.
- Overwrite is permanent. Local deletion uses the Recycle Bin; WSL deletion is permanent. Undo does not cover every operation and is lost on exit.
- Session saving is off by default. Editor drafts are separate, enabled by default with exclusions, and cannot guarantee recovery of the last edit.

Read [safety and limitations](docs/en/safety.md) before using important data.

## Developer information

piero is a Windows-native C17 application built with CMake, clang-cl or MSVC.
Start with [development](docs/en/development.md) for prerequisites, build commands and validation.
[Architecture](docs/en/architecture.md), [coding rules](CODING_STYLE.md) and
[manual validation](docs/en/manual-validation.md) cover the engineering details.

## License

Project-owned code and documentation use the [MIT License](LICENSE).
Bundled third-party components retain their own [license notices](LICENSES/);
see [licensing](docs/en/licensing.md).

Developed by [cyan-cs](https://github.com/cyan-cs).
