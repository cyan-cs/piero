# Changes

## Unreleased — development preview

This section describes the current implementation, not a qualified stable release.
See [known limitations](docs/en/safety.md) and the
[manual validation guide](docs/en/manual-validation.md) for validation scope.
Past design records are available in Git history.

### Navigation and inspection

- Miller columns, bounded directory prefetch and in-process Navigation Memory.
- English/Japanese UI, Standard/Nano/Vim/Custom keys, mouse navigation and themes.
- Session-local tabs, saved places, and opt-in session continuation with `--continue`.
- Fuzzy name search, recursive name/metadata search and duplicate-file candidates.
- On-demand folder sizes, SHA-256 calculation and NTFS details.
- Restart Manager inspection with guarded close/terminate and explicit retry.
- Open Terminal Here for local folders.
- Git status markers and a read-only editor diff against HEAD, using Windows Git
  or the WSL distribution's Git. Git is optional; no commit or push interface.

### Preview and editing

- Bounded text preview, formatted Markdown and CSV/TSV tables.
- Image/PDF preview enabled by default, decoded in an AppContainer with resource
  limits and no unisolated fallback. Sixel requires terminal capability checks.
- Read-only ZIP, 7z, TAR and compressed-stream browsing; ZIP extraction uses
  staging, validation and the copy engine. Archives themselves are not modified.
- An encoding/line-ending-preserving editor with asynchronous reads/saves,
  literal find/replace, Vim operators, Visual mode, registers and macros.
- Live Markdown editor preview and optional Tree-sitter syntax colouring.
  Grammar files must match the executable's build; the package scripts do not
  currently include them.
- Replacing editor/settings saves retain originals as `name~` through `name~9`.
  Draft recovery is best-effort, with size/path/name exclusions.

### File operations and integrations

- Create, rename, copy, same-volume move and bulk rename.
- Recycle Bin deletion, cross-volume move, overwrite and Keep both choices.
  Copy uses SHA-256 read-back verification by default. Overwrite permanently
  removes the old file and cannot be undone by piero.
- Session history and Undo for supported operations, including completed bulk
  renames. Interrupted batch recovery asks before attempting rollback.
- Windows file clipboard copy in both directions; cut remains unavailable.
  Real Explorer interoperability is not yet qualified.
- WSL-specific create/rename/copy/move/delete/save operations and limited Undo.
  WSL deletion is permanent. SFTP browsing and editor viewing remain read-only.
- Persistent state uses `piero`, with compatibility for existing `wfile` state;
  internal `wf_` identifiers and recovery markers retain their names.
- Opt-in release-information checks; downloads and installation remain manual.
- Portable ZIP and per-user Windows 11 installer build support. Signing and
  clean-machine installation/uninstallation require separate validation.

### Implementation and documentation

- Shared worker lifecycle, on-demand worker startup/release, and idle release of
  image resources and the archive cache.
- MIT project license, retained third-party notices, paired user/contributor
  guides, build/test scripts and CI checks.
- Documentation now distinguishes current behavior from superseded proposals
  and historical test evidence. Automated test results do not certify real
  terminal, IME/JIS, cross-volume or power-loss behavior.
