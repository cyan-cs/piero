[English](features.md) | [日本語](../ja/features.md)

# Features

## Navigation and presentation

Miller columns show the parent folder, current folder and preview. Cursor and
scroll positions are remembered during a run; filters are not restored. Folder
listing and prefetch have [bounded resource limits](architecture.md#navigation-limits).
Remote/cloud automatic preview remains disabled.

Going to the parent of a drive root (or of a WSL distribution) opens "This PC",
which lists the drives and the registered WSL distributions with the one you came
from selected; Enter goes back in. Nothing is started to list it, and nothing can be
created, copied, moved or deleted there.

The interface supports English and Japanese, Standard/Nano/Vim/Custom keymaps,
8 built-in themes (Spade, Heart, Diamond, Club, Joker, King, Queen, Ace) and up to
8 custom themes. Mouse clicks, double clicks and the wheel work in the file
manager. See [keybindings](keybindings.md) and [configuration](configuration.md).

Tabs (`t`, `w`, `[`, `]`) keep up to 8 folder/cursor positions during a run;
they are not part of `--continue`. Places (`m` then a letter, `'` then a letter)
save folders in `[places]` in the configuration. Session persistence is off by
default; enable `[session] persist=true` before using `--continue`.

## Preview and archives

Text preview limits reads and sanitizes terminal control characters. Markdown
preview renders headings, lists, emphasis, code, links and tables. HTML stays
text; links and images are not fetched. Reference-link destinations have a
combined 4 MiB expansion budget; references beyond it stay literal.
CSV/TSV previews align columns, with
CSV-style double-quoted fields, up to 200 records, 64 columns and 32 display cells
per field. This is a tolerant preview, not an RFC 4180 validator: an unclosed
quoted field is accepted through the end of the input.

Image and PDF preview is on by default. WIC handles PNG/JPEG/GIF/BMP/TIFF/ICO;
WebP/HEIF/JXL depend on installed codecs. PDF uses Windows.Data.Pdf. Decoding
runs in an AppContainer with no capabilities and bounded memory and time;
failure does not fall back to decoding in the UI process. Automatic Sixel requires
confirmed terminal capabilities and pixel dimensions; text-cell graphics are
the fallback. Explicit Sixel overrides part of detection; see [configuration](configuration.md). These checks do not guarantee detection of every damaged file.

Archives can be browsed without modifying them: ZIP/ZIP64, 7z/solid 7z, TAR,
`.tar.gz`/`.tgz`, `.tar.xz`/`.txz`, `.tar.zst`/`.tzst`, and single `.gz`, `.xz`,
`.zst` streams. Format detection uses content. Password-protected and nested
archives are not supported; links are listed without being followed.
RAR/CAB/ISO/cpio/bzip2/lz4 are not built in.

ZIP entries can also be extracted: in Standard keys, `y` stages selected entries
(or the entry under the cursor), then `p` in a local folder extracts them.
Other formats are browse-only. Extraction validates names and data in a hidden
`.wfextract-*.dir` staging folder, then uses the copy engine to publish the
results. Existing folders are merged and file collisions use the copy prompt.
Limits are 16 GiB per file, 64 GiB total and 200000 entries; the destination
needs space for staging and copying. A crash may leave the staging folder;
inspect the result and retained files rather than assuming cancellation removed
everything. No archive-writing operation is offered.

## Editor and Git

The editor supports UTF-8, BOM UTF-8/UTF-16 and a configured code page. Uncertain
decoding requires confirmation and a round-trip check. It preserves per-line
LF/CRLF/CR, BOM and final-newline presence. Defaults are a 32 MiB file limit
and a 16 MiB Undo budget. Find and replace are literal, without regular expressions.
Reads and saves run on a worker; Esc cancels reads and saves before replacement
begins. Draft recovery has size and filename exclusions and depends on a
successful checkpoint; see [configuration](configuration.md).

The editor has built-in colouring; optional Tree-sitter grammar packs add
colouring to the editor and preview. dotenv uses a built-in lexer. See
[syntax colouring](syntax.md) for formats, installation and trust requirements.
Vim mode includes operators, motions, text objects, Visual mode, registers,
macros and `.`; Ex ranges, marks and `:s` are unavailable.

Markdown files have a live preview pane from 100 terminal columns in automatic
mode. `Alt+M` toggles it; `[editor] markdown_split=auto|on|off` controls the default
(`on` permits it from 60 columns). Footnotes, math, front matter and syntax
colouring inside code blocks are not rendered.

Git worktrees show row markers: `M` modified, `U` untracked, `A` added, `D` deleted,
`R` renamed and `!` conflict. The file name takes the colour of its marker, and a
folder that holds a changed file somewhere below it is tinted (`[git] folder_color`).
At 100 columns or more, the editor can show a
read-only diff between HEAD and the current buffer, including unsaved edits.
Windows repositories use `git.exe` on PATH; WSL repositories use the distribution's
`git` through `wsl.exe`. No other backend is tried when that Git is unavailable.
Queries run asynchronously and use read-only Git commands. There is no staging,
commit or push interface.
The Markdown preview takes precedence over the Git pane; hide it with `Alt+M`
to show the diff. HEAD blobs and flattened editor text are each limited to
4 MiB; highly changed inputs can also exceed the diff limit. Comparison ignores
CRLF/LF differences and final-newline presence. HEAD bytes are not decoded to
the editor's UTF-8 representation, so non-UTF-8 files may show encoding differences
as content changes.

## Search and inspection

- `/` filters the current list; Tab in the filter prompt searches below the
  current folder. Terms include `ext:txt`, `size>10k`, `size<2m`,
  `newer:2026-10-01` and `older:2026-01-01` (dates are UTC).
- `s` performs fuzzy name search in the current list.
- `F` groups duplicate candidates by size, a prefix SHA-256 and a full SHA-256.
  You choose which files to recycle; the UI refuses deletion of an entire group.
  This does not guarantee protection against changes made by other programs.
- `#` computes a local file's SHA-256 on request; `D` counts folder sizes.
- `i` shows details, including available NTFS link, stream and volume metadata.
- `L` uses Restart Manager to inspect programs holding a file. Closing or force
  termination requires confirmation; protected or unverifiable processes are
  refused. Retrying returns to the operation's normal checks.

Recursive search, duplicate search, hashing and folder-size calculation are
bounded worker operations. They skip links and unsupported locations; recursive
search and duplicate search require local folders. Esc cancels work. See
[keybindings](keybindings.md) for result-window operations.

## File operations and external tools

Create, rename, copy, same-volume and cross-volume move, Recycle Bin deletion and
bulk rename are implemented. A new name such as `test/hello` creates the file
(or folder) `hello` inside the folder `test`, making `test` if it is missing;
an existing file or link on the way is not entered, and `..` is refused. Copy verifies by SHA-256 read-back by default;
explicit `verify=none` disables that check. A cross-volume move verifies the copy
before sending its source to the Recycle Bin. Encrypted, linked and online-only
sources are refused or skipped. Copies do not preserve ACLs, extended attributes
or encryption/compression state from the source. Overwrite retains the
destination's access permissions.

File collisions offer Skip, Keep both or Overwrite. Overwrite permanently removes
the old file; it is not recycled or recoverable through Undo. Copy merges existing
folders; Move does not. Replacing editor/settings saves keep the original under
the first free name from `name~` to `name~9`; all 9 occupied means save is refused.
Save As does not overwrite. See [known limitations](safety.md).

`J` shows the last 32 changes of the session. Undo (`u`) retains up to 16 entries
and covers rename, same-volume move, newly created copies and completed bulk
renames, subject to identity and destination checks. It does not undo Recycle,
Create, overwrite or cross-volume Move. Bulk cancellation attempts rollback;
the result identifies incomplete rollback. Neither history nor Undo is persisted.

`Y` publishes a file copy to the Windows clipboard; `V` imports a file copy.
Cut, virtual files and imports from multiple source folders are refused. Integration
has automated receiver tests; real Explorer interoperability remains unverified.
`!` opens a terminal in the current local folder, trying Windows Terminal,
Windows PowerShell and cmd.exe in that order. External file launch checks identity
and confirms potentially executable or unknown types, using a private neutral
working folder by default.

## WSL and SFTP

Go (`g`) accepts `wsl:<distribution>[/path]` and `sftp:<host>[/path]`.
WSL uses separate operations because its paths do not provide the Windows file
identity required by the local copy engine. It supports create, rename, copy,
move within a distribution, delete and editor saves. Copies can cross between
Windows and WSL; moves cannot. WSL copy operations are bounded to 20000 entries,
48 levels and 256 MiB of data. Copy and move do not replace existing names.
Editor saves retain a backup but write in place; a crash can leave partial
contents. See [safety](safety.md). WSL deletion is permanent, with a separate
confirmation and no Recycle Bin. WSL Undo covers the
last create, rename or move. Copies do not preserve Linux mode or owner.

SFTP uses your `ssh.exe` and SSH configuration with a previously accepted host
key and noninteractive authentication (for example a key available to SSH).
Password and keyboard-interactive prompts are disabled. It is read-only: browse
and view files, including in the editor. Saving back to the server is unavailable;
Save As can create a new local Windows file. These backends have different safety
and metadata limits
from local Windows folders. Real external-environment checks are described in
[manual verification](manual-verification-procedures.md); automated tests do not
qualify every distribution or server.
