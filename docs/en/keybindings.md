[English](keybindings.md) | [日本語](../ja/keybindings.md)

# Keybindings

## Active profile

`?` and Settings show the active bindings generated from the command tables.
This guide describes common operations and how to generate the full built-in
reference below. Standard is the unselected/default profile; first-run setup offers Nano/Vim. Never assume
Standard transfer keys apply after selecting Nano or Vim.

| Standard | Action |
| --- | --- |
| `j` / `k`, arrows | Cursor |
| `h` / `l` | Parent / enter directory |
| `b` / `f` | Back / forward |
| Space / Esc | Mark; Esc cancels one thing at a time: a running count, the marks, the filter, then a pending copy or move |
| `/`, `.`, `r` | Filter, hidden entries, reload |
| `/` then Tab | Search this folder and its descendants |
| `F` | Find duplicate-file groups |
| `g`, Ctrl+L | Go; Ctrl+L preselects current path |
| `g` then `wsl:<distribution>[/path]` | Browse and modify a WSL location |
| `g` then `sftp:<host>[/path]` | Browse an SFTP location (read-only) |
| Mouse | Click to select, double click to open, wheel to scroll |
| Pasting text | Detected paste is treated as text; see limitations below |
| `i`, `v`, `P` | Details, full preview, explicit preview request |
| `e`, `,`, `?`, `q` | Editor, Settings, help, quit |
| `y` / `x`, `p` | Stage copy / move, then paste |
| Delete, `u` | Recycle Bin after asking; Undo of rename, move, copy and bulk rename (the newest change first, after a question; a bulk rename is rechecked as a batch and rollback failures are reported) |
| `J` | Show change history and retry failed transfers |
| `?` | Help by category (Left/Right); the search box at the top takes what you type (a key, an action name or a word), shows how many match and marks the match; keys that are exactly the text come first, and each row names its category; Esc clears the search, then closes; asked from a preview it opens at the preview's keys |
| `B` | Backups (`name~` to `name~9`) of the file under the cursor: open one in the editor, or send it to the Recycle Bin |
| `Y` | Publish selected paths as a copy |
| `V` | Copy clipboard files into this folder |
| `L` | Inspect locks, request close or retry an operation |
| `s` | Find names by letters appearing in order |
| `M` | Preview and apply a rename pattern |
| `!` | Open a terminal in the current local folder |
| `#` | Compute the selected file digest on request |
| `t`, `w` | Open or close a tab (not persisted) |
| `[`, `]` | Select the previous or next tab |
| `m` then a letter, `'` then a letter | Save or recall a folder under a letter |
| `D` | Count folder sizes; Esc stops |
| `z`, `C` | Pause/resume queue; cancel |
| `a`, `%`, `d`, F2 / `R` | Create menu, file, directory, rename |

## Detailed operations

### Folder search

Widen the filter to this folder and below: the text becomes a search of the folders
under this one, nearest first, and a window lists what it finds as it finds it. Words
(all must be in the name, any case), `"a b"`, `ext:txt`, `size>10k`, `size<2m`,
`newer:2026-10-01`, `older:2026-01-01` (days are UTC).

In the window: Up/Down, PgUp/PgDn, Home/End; Space marks; Ctrl+A marks all; Enter shows
the result in its folder; `y` and `x` copy and move the results of 1 folder; Delete
sends results of any folders to the Recycle Bin after 1 question; Esc stops the search,
and Esc again closes the window.

### Duplicate files

List the files below this folder that hold the same bytes (same size and same SHA-256,
found in 3 steps: by size, by the first 64 KiB, by the whole file). A window lists the
groups, each file with `[n]` for its group and with its folder.

Space marks the copy you do not want (the last file of a group cannot be marked); Delete
sends the marked ones, or the one under the cursor, to the Recycle Bin after 1 question
and never a whole group; Enter shows a file in its folder; Ctrl+A, `y` and `x` are not
offered; Esc stops, and Esc again closes. The search itself only reads files.

Command `fm.dupes`.

### WSL operations

Go into a WSL distribution. There you can create, rename, copy, move (inside the same
distribution), delete and save; a name that exists is never replaced, and deleting is
permanent (no Recycle Bin). A copy attempts to retain the modification time and reports failures, but does
not preserve the Linux mode or owner. `u` undoes the last create, rename or move.

### SFTP browsing

Browse a server with your own `ssh.exe` (host name from your ssh configuration; the host
key must already be known). The editor cannot write back to the server; Save As
can create a new local Windows file.

### Mouse input

Click an entry to put the cursor on it; double click to open it; click an entry in the
parent column to go to it (a folder is entered, a file is shown in its own folder); the
wheel scrolls the list or the preview, whichever is under the pointer, 3 lines at a time
(the editor does not use the pointer).

### Yes / No questions

A question is answered by typing `Y` or `N`, after a short pause in which keys are ignored.
On a plain Yes / No question, Left or Up chooses Yes and Right or Down chooses No; the
choice is marked with `>`, and Enter takes it. Enter alone is always No, as is Esc, and
the choice starts at No. Questions with other answers (skip, overwrite) are answered by
their letters only.

### Pasting text

Detected pasted keys are not run as commands in the file list. In a prompt they are
text, but a pasted Enter or Tab never confirms and the lines after it are not taken; a
question is never answered by them.

The console host does not pass on the bracketed-paste markers, so a paste is recognised
by how it arrives: 3 or more different keys in 1 read (or an Enter with another key),
and whatever follows within 30 ms. 1 or 2 pasted characters cannot be told from typing.
A pasted line break counts 1 time.

### History and retry

History of the last 32 changes of the session: copy, move and delete jobs (every failure
with its full path), renames, moves on one volume, new files and folders, bulk renames.
Under each row: "u: undo this", "can be undone after the newer changes", "undone", "can
no longer be undone" or "cannot be undone" and why.

`u` there is the same Undo as in the list (the newest change, after its question); `r`
puts just the sources that failed back into the transfer slot and opens their
destination (pasting asks everything again). The history itself is not persisted;
Undo and retried operations can change files.

### Copy to the Windows clipboard

Put the marked files (or the entry under the cursor) on the Windows clipboard as a copy,
so Explorer and other programs can paste them. Only the names are published; nothing is
read, copied or changed. Verified with a test receiver, not with a real Explorer.

### Paste from the Windows clipboard

Paste the files on the Windows clipboard (Explorer's Copy) into this folder, by the
ordinary copy: the same checks, the same question about names that exist. A cut from
Explorer, files of several folders, a network share, and anything that is not real files
are refused and change nothing. `X` (cut to Explorer) is not built.

`Ctrl+C` is the program's interrupt and Windows Terminal takes `Ctrl+V`, so the file
clipboard is on the shifted keys.

### Programs holding a file open

Restart Manager lists programs holding the selected file open. Use it after a
failed copy, move, delete, rename or editor save, or for the file under the cursor.

| Key | Action |
| --- | --- |
| `R` | Refresh the list |
| `C` | Ask the program to close |
| `K` | Force termination after two confirmation questions |
| `T` | Return the failed operation to its normal entry; recheck everything, without starting automatically |
| `Up` / `Down` | Select a program |
| `Esc` | Close the window |

Force termination refuses piero itself, services, critical/protected/system
processes, other users' programs, and processes whose PID and start time cannot
be matched. The command ID is `fm.locks`.

### Fuzzy search

Search the names in this list by a few letters (fuzzy; Nano also `Ctrl+W`). Results
appear best first as you type, with the letters that matched underlined, and the cursor
follows the chosen result.

The letters only have to appear in order (`cfg` finds `config_file.go`); case does not
matter, and Hiragana finds Katakana; accents and composed letters are not unified.
Up/Down choose (past the first 64 results it loads more), Enter leaves the cursor on the
entry (it does not open it), Esc puts the cursor and scroll back.

Nothing is read from the files or changed. `/` stays the filter, which hides what does
not contain the text.

### Bulk rename

Rename the marked entries (or the one under the cursor) by a pattern, after a preview.
Fields: template (`{name}` the name, `{n}` / `{n:4}` a number), find, replace, first
number, step, name case, extension case; `Tab` / `Shift+Tab` change field, `Enter` goes
to the preview, `Esc` cancels.

The preview lists every row as before -> after with the reason for each refused row; a
batch with a refused row, or with nothing to change, cannot be run and changes nothing.

`Enter` on a good preview asks one question (answer `y` after the short guard); `Esc`
requests cancellation and rollback; inspect the result for any names that could not be
restored. The result says "Done" only when every rename was made.

A crash leaves a journal in the state folder (`batch\`); the next start asks whether to
recover it (never by itself). A completed batch can be undone with `u`, after identity
and name checks. Command `fm.bulk_rename`.

### Open a terminal

Open Terminal Here: a new terminal in the folder on screen (the folder of the tab you
are in, never the entry under the cursor). Windows Terminal (`wt.exe -d`) when it can be
started, else Windows PowerShell, else cmd.exe.

Only for a folder on a local drive; a share, an archive, SFTP, WSL, a path of 260
characters or more, a folder that is gone or cannot be entered is refused with a
message. Nothing is elevated and piero goes on. Command `fm.terminal`. See
[features](features.md)

### SHA-256

Compute the SHA-256 of the file under the cursor on request; hashing does not
start automatically while browsing. The status line shows the progress and then the digest; `Esc` stops it; the
details window (`i`) shows the digest for as long as the file is the one that was read,
by size and last write time. A file that changed while it was read gets no digest.

Folders, links and files that are not on a local drive are refused.

### Tabs

Open a tab at this folder, or close this tab. A tab is a folder and the name under its
cursor; there are at most 8; a tab strip appears in the title row only when there are 2
or more tabs. Tabs are for this run and are not saved.

### Switch tabs

The previous and the next tab (they wrap around). Ctrl+Left and Ctrl+Right do the same
in the Standard and Vim profiles; in Nano Ctrl+Right enters a folder, so only the
brackets are bound.

### Saved places

Keep the folder you are in under a letter a to z, and go to the folder kept under a
letter. `'` lists the places first; Esc or another key cancels. Places are written to
the configuration file (`[places]`) and are there in the next run.

### Directory sizes

Count the size of every folder in the list (they show at the end of each folder's row;
Esc stops, keeping what was counted, marked `+`; `r` forgets them). Links are not
followed. The details window (`i`) gives files, folders and bytes.

## Nano, Vim and Custom

Nano: arrows or Ctrl+P/N, Enter opens, Left returns, Ctrl+F filters,
Ctrl+G helps, Ctrl+X or `q` quits. Alt+6 stages Copy, Ctrl+K stages Move,
Ctrl+U pastes, Alt+U undoes. `hjkl` and `y/x/p` are not Nano navigation/transfer.
Vim adds `gg`, `G`, counts up to 9999, `yy` Copy, `dd` Move (never deletion),
`p` paste and `:` commands. Sequences time out after 1.5 seconds.
The command line has no shell escape or arbitrary program execution.
Custom bases on Nano/Vim; conflicts and terminal-indistinguishable keys are rejected.

## Editor and settings

Editor Ctrl+S saves, F2 helps. Piero Ctrl+X/Esc closes; Nano Ctrl+X closes.
Ctrl+F finds; Piero Ctrl+H replaces; Nano Alt+R replaces. Piero Ctrl+Z/Y
undoes/redoes; Nano Alt+U/E does so. Vim Normal uses `i` to insert, Esc returns,
`:w` saves, `:wq` closes only after success; `:q!` discards only that buffer.
Editor `dd` cuts a line, unlike file-manager `dd`, which stages Move; in the editor `d` waits for a motion.
Use active help for context-specific clipboard and buffer commands.
`Alt+M` shows or hides the Markdown preview pane of a `.md` file (Standard, Nano and Vim keys, normal and insert mode; Vim also `:preview`); the command is `ed.md_preview`, so Custom can rebind it.
Settings Ctrl+S saves; Esc asks about changes with `s`/`d`/`c`.

Short questions in the editor and Settings (save changes, reload, delete, reset) take their letter, or Up/Down and Enter. The selection always starts on Cancel, so Enter alone saves and discards nothing; this is the way to answer with a Japanese IME on, when a letter key starts a composition. Full-width letters, kana and keys the IME was handling never answer a question.
File-operation confirmation prompts require the displayed confirmation letter;
Enter cancels them. The editor/Settings choice dialogs above also accept Enter
after you select a choice.
IME and JIS character matching require real terminal qualification.

## Custom configuration

Use Settings > Keyboard to change bindings. In a configuration file,
`[keyboard] profile=custom` selects Custom; `[keyboard.custom] base=nano|vim`
selects its base. Overrides use `[keyboard.custom.<context>]`, with command IDs
such as `fm.nav.down` as keys and key strings such as `k, Up` as values.
Use `[keyboard] bindings_version=2` for the current Nano/Custom defaults;
older versioned configurations can intentionally retain their older navigation.
The generated reference lists context names, command IDs and default bindings,
not your saved overrides. Check the running application's help after editing.
See [configuration](configuration.md) for file rules and validation.

Comma-separated keys are alternatives; a sequence such as `g g` means consecutive
keypresses. Esc cancels an incomplete sequence. Counts apply to supported Vim
navigation, never Copy/Move/Recycle/paste. Terminal aliases such as Ctrl+I/Tab
and Ctrl+M/Enter cannot be assigned independently; Ctrl+C is not assignable.

## Vim command line

Open the file manager's command line with `:`. Its syntax is:

```text
:go <path>
:sort name|size|modified|extension [asc|desc]
:set hidden on|off
:filter <text>
:clear-filter
:reload
:settings
:help
:q
```

Quote a path containing spaces. Sorting by creation time is not offered.
Quit follows the normal active-job confirmation. `:!` and shell execution
are unavailable.

The editor also supports `:w`, `:wq`, `:q`, `:q!`, `:replace`, `:buffers`,
`:bnext` and `:bprev`. Normal quit asks about changes; forced quit discards
only the current buffer. In Normal mode `d`, `y` and `c` take a motion (`w`, `b`, `e`,
`$`, `0`, `h`, `l`, `j`, `k`, `G`, `gg`, `f`/`t`/`F`/`T` with a character, a text object
such as `iw`, `aw`, `i"`, `a(`, `i{`, or the same letter again for the line),
with a count before either (`3dd`, `d2w`); `D` and `C` go to the end of the line.
Visual mode (`v`, `V`) selects with the motion keys and then `d`, `y` or `c`
act on it. `"a` before a copy, cut or paste names a register, and `qa` ... `q`
records a macro that `@a` plays. A capital letter (`"A`) adds to the register.
`.` repeats the last change (a count repeats it that many times). Use `:replace` instead of Vim's unsupported `:s` syntax.

## Generate the full built-in reference

From a configured development build with testing enabled, run:

```powershell
cmake --build out/dev --target dump_keys
out/dev/dump_keys.exe
```

Replace the build directory if yours differs. The diagnostic target
`tests/dump_keys.c` prints the Standard/Nano/Vim file-manager, preview and
editor contexts directly from the command and keymap tables. Output is English;
each row contains keys, action and configuration command ID. `(none)` means
that command has no default binding in that context. This is not a dump of
Custom overrides, Settings/dialog keys or the running application's state.
Keep redirected output under ignored `out/`; do not maintain another copied
key list in the repository root.
