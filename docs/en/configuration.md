[English](configuration.md) | [日本語](../ja/configuration.md)

# Configuration

## File and editing

The default file is `%APPDATA%\piero\config.ini` (or `%APPDATA%\wfile\config.ini` if only that older folder exists). `--config <path>` requires
an existing valid file. INI is strict UTF-8, optional BOM, with no inline
comments, quotes or escapes. Unknown keys/comments survive Settings edits.
Conflicting external changes refuse save; `r` reloads Settings. Language/theme/
color preview live; options marked next start need restart.
Invalid schemas cannot become writable unversioned files; future schemas are
read-only. Equivalent section aliases are canonicalized on save.
Settings saves retain the original as `config.ini~`, or the first free name
from `config.ini~2` to `config.ini~9`. Saving is refused when all 9 names are occupied.

## Common options

```ini
[meta]
schema_version = 1

[general]
language = auto

[keyboard]
profile = standard

[appearance]
theme = spade
color_mode = auto

[preview]
images = true
graphics = auto
image_quality = high
image_style = default
directory_preview = true
image_cache_mib = 32
text_max_kib = 256
text_fallback_encoding = cp932

[updates]
enabled = false
consent_recorded = false

[copy]
verify = sha256

[editor]
enabled = true
keymap = piero
backup = true
max_size_mib = 32
undo_mib = 16
```

`language` accepts `auto`, bundled codes `en` and `ja`, and additional language
codes described in [translations](translating.md). Auto uses Windows' preferred
UI language list to choose an available match, falling back to English.
CLI startup/help remains English.
`text_fallback_encoding` accepts `cp932`, other `cp<number>` code pages, `utf8`
or `utf-8`; bare numbers are invalid and produce a warning, retaining the default.
`profile`: `standard|nano|vim|custom`; `[editor] keymap` accepts only `piero|nano`;
Vim editing is selected through the keyboard profile, not that legacy setting.
`theme`: `spade|heart|diamond|club|joker|king|queen|ace|custom`;
`color_mode`: `auto|truecolor|256|16|none`. `NO_COLOR` and `TERM=dumb` disable color in auto.
Custom themes use `[theme.custom.<id>]`, `name`, `base`, and `#RRGGBB` colors.
Settings can duplicate/edit/rename/delete custom themes; import/export is absent.

## Display and listing options

`[terminal] ambiguous_width=1|2` (default `1`) is how many cells your terminal
draws an "East Asian Ambiguous" character in: accented Latin letters, Greek,
Cyrillic, arrows, circles and other symbols, and private-use icon glyphs. Set `2`
only if your terminal is set to draw those wide; piero then counts them as 2 cells, so that
columns, the cursor and long names match the terminal. Match this setting to the
terminal rather than inferring it from the UI language. The default is 1 cell. Combining marks stay
0 wide and CJK characters stay 2 cells either way. Frames and key help use
plain ASCII and are not affected. The setting is read once when piero starts.

`[files] directories_first=true|false` (default `true`) lists folders before
files. With `false`, folders and files are ordered together by the sort key
(name, size, date or extension), also after you change the sort with `S`.

`[git] folder_color=true|false` (default `true`) tints a folder that holds a changed
file in a Git repository. File names are always coloured by their Git marker.
It is also in Settings > Files.

## Preview and safety options

### Images and graphics

`images=true` (the default) shows pictures and PDF pages in the preview, decoded in the isolated host; `images=false` turns them off at next start and the preview then says so.

Input is limited to 128 MiB and source images to 256,000,000 pixels. Images may be reduced during decoding; success depends on the codec and the host's memory (512 MiB) and time (12 s) limits. Structural checks reject some damaged inputs, but are not complete corruption detection.

`graphics=auto|sixel|text`: auto requires advertised Sixel/erase support, pixel
dimensions and a known palette. Explicit Sixel bypasses the advertised-support
check, but still requires measured pixel dimensions. Use it only on a terminal
known to support Sixel; it can corrupt the display on an unsupported terminal.
Text mode disables Sixel.

`image_quality=high|balanced|fast`; `image_style=default|halfblock|ascii|off`.

### Copy verification and backups

`verify=none|sha256|hmac-sha256`; the default is `sha256`. Explicit `none` disables
copy verification; cross-volume moves still require read-back verification.

No option enables permanent deletion in local Windows folders (WSL deletion is permanent); overwriting an existing name is chosen per job, never by a setting.

`backup=false` is rejected as a retention policy.

### Drafts and Markdown editing

`[editor] recovery_draft=true|false` (default `true`): while you edit, unsaved text is written to a draft under `%LOCALAPPDATA%\piero\drafts` (`wfile` if that is the folder in use) every few seconds and offered back the next time the file is opened. Files named like secrets, files on network paths, files in `.ssh`, `.gnupg` or `.aws` folders and text over 2 MiB are excluded. The limit
applies to the UTF-8 draft text, not the original encoded file size. `false` writes nothing until you save.
Drafts contain plain-text copies outside the original file's folder; they are not encrypted.
Filename exclusions are heuristic; file contents are not scanned for secrets.

`[editor] markdown_split=auto|on|off` (default `auto`): a Markdown file is edited in 2 panes (text left, preview right). `auto` shows the preview from 100 columns, `on` from 60, `off` never; `Alt+M` changes it for the session. Not in the Settings screen.

### External launch and automatic reads

`[launch] working_directory=neutral|file_dir` defaults to `neutral`.
Remote/cloud automatic reads remain disabled even when configuration requests them.

See `src/config/wf_config.c` for accepted keys/ranges, [keybindings](keybindings.md)
for Custom command names and the full-reference generator, and [safety](safety.md) for retention and race limits.
Use the current schema and defaults above.

### Update checks

`[updates] enabled` permits automatic release-information checks only after
`consent_recorded=true` has been saved. Both default to false. Invalid settings
disable automatic checks. Settings > Updates also allows a manual check while
automatic checks are off. No update file is downloaded or installed. See [updates](updates.md).

## Tabs and places

Tabs are for the run you are in and are not saved. Places are: `m`, then a letter, keeps the folder you are in under that letter, and `'`, then the letter, goes there. They are written to this file by the same save as the settings, as a section older versions do not know:

```ini
[places]
a = C:\Work
b = D:\Photos\Archive
```

The key is one lowercase letter, the value is the folder as UTF-8 text; a letter that is repeated keeps the first folder, and a value that is empty, has a control character or is too long is ignored with a warning. A folder inside an archive or an SFTP place is not kept, and nothing is written when the file is of a newer version.

## Continuing where you left off

`piero --continue` puts back what the last run saved: the folder, the cursor, the
scroll, the selected names and the preview. Saving is off until you turn it on:
`[session] persist=true`, or Settings > General (a change applies at the next
start). While it is on, the state is written 1 second after it changes, by a
thread of its own that the keys never wait for, and once more when you quit. It
holds paths and names only, never the text of a file.

- A folder or `--setup` cannot be combined with `--continue`; conflicting
  startup requests are refused.
- If nothing was saved, the saved data is damaged, or the format is unsupported,
  piero reports it and starts as usual. Damaged or unsupported data blocks session
  writes, including when persistence is enabled without `--continue`. An unreadable
  header also blocks writes. Preserve the files before using `piero --forget-session`
  to clear them and allow saving again. See [troubleshooting](troubleshooting.md#damaged-session-data).
- If the saved folder is gone, the nearest folder above it that exists is shown.
  A network share is asked about, as always.
- Only 1 piero writes at a time (the one that holds `session.lock`); another one
  reads, and says that it does not save. The lock is released by the system when
  that piero ends, however it ends.
- The files are `session.a`, `session.b` and `session.lock` in the state folder
  piero already uses (`piero`, or `wfile` where that is in use). `piero --forget-session` empties the saved session: its content is gone, the files stay.
- When the editor had the screen, `--continue` opens it again on the same file
  (if it is in the saved folder) with the cursor and the view where they were. The
  text is not in the session: unsaved text is the editor's draft, and the editor
  asks about it as it always does (restore or discard). The current file only; the
  Vim mode is not restored. A file that changed meanwhile is read as it is now,
  and saving it keeps its own conflict check.
- After a crash, only the last successfully written checkpoint can be restored.
  The 1 second interval is a scheduling target, not a bound on data loss. Unsaved
  text is recoverable only if its draft was successfully written.
- Putting a session back never writes to a file you own and never answers a
  question for you. Operation recovery is separate. Archives resume at their
  containing folder, SFTP locations are not saved, and at most 256 selected names
  are retained.
