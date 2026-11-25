[English](manual-verification-procedures.md) | [日本語](../ja/manual-verification-procedures.md)

# Manual verification on real terminals and external environments

Steps for items that automated tests (including ConPTY) **cannot** confirm.
Every item here is **NOT RUN** until it is performed and recorded.
A ConPTY test that sends already-committed text is a different test from
converting text with a real IME.

Common rules:

- Use only disposable areas and synthetic data. Never run destructive trials on user data, real drives or real SSH hosts.
- Record PASS / FAIL / SKIP / NOT RUN. Never mark an item PASS that was not performed.
- Evidence: build hash (`piero.exe --version` and SHA-256), Windows build number, terminal and version, keyboard (JIS/US), IME name, date.
- On failure: screenshot, a listing of the logs and receipts under `%LOCALAPPDATA%\piero` (or the older `%LOCALAPPDATA%\wfile` if in use) (do not edit or delete them), and the step number.
- Do not touch the old journal `%LOCALAPPDATA%\wfile\copy-journal.txt`.

## A. Windows Terminal (real terminal)

Prerequisites: x64 Windows 11, Windows Terminal (PowerShell 7 tab), the `piero.exe` under test.
Unless a row specifies otherwise, complete setup and use the English UI with the
Standard keyboard profile. Use a fresh disposable destination for each transfer.
The status text below is an English label; record the actual text in your build.

| # | Steps | Expected | Evidence on failure |
|---|-------|----------|---------------------|
| A1 | Run `piero.exe --version` | One line starting with `piero ` | Full output |
| A2 | Start `piero.exe` in a disposable folder | Three panes, status line READY | Screenshot |
| A3 | Resize the window (down to 40x12) | No corruption; `Current` visible | Screen at each size |
| A4 | Hold `j` / `k` (key repeat) | The cursor follows without visible lag | Video, or key count and screen |
| A5 | Do nothing for 30 s | No flicker; Task Manager shows about 0% CPU | Screenshot |
| A6 | Copy a ~1 GiB synthetic file: `y`, go elsewhere, `p` | Progress shown, ends with `COPY done` | Screen, SHA-256 of both files |
| A7 | Press `z` during A6 | `PAUSED` appears and progress stops; `z` resumes | Screen, temp file size (twice) |
| A8 | During A6 press `z`, then `C` | Cancellation reported; original unchanged. Inspect completed items and temporary files against per-item results | Destination listing |
| A9 | On a Sixel terminal, put the cursor on an image | Image shown; text fallback otherwise | Screen |
| A10 | On a terminal with synchronized output (DEC 2026), scroll heavily | Less flicker; a terminal without it still works | Comparison video |

## B. Japanese IME (Microsoft IME)

The application receives **committed text** only. Uncommitted composition
(the underlined candidate) is owned by the terminal and never reaches the
application. The application treats committed characters as ordinary input.
The Enter that commits an IME conversion is consumed by the terminal/IME; it
is not an operation Enter.

Prerequisites: Windows Terminal, Microsoft IME (Japanese), JIS or US keyboard, `piero.exe` in a disposable folder.

| # | Steps | Expected | Evidence on failure |
|---|-------|----------|---------------------|
| B1 | `a` then `%` (new file). With the IME on, type `こんにちは`, press Space to convert, **Enter to commit** (first Enter) | No file is created yet; the field holds `こんにちは` | Screen; FAIL if the file was created |
| B2 | Press Enter again (second) | The file is created and `Done:` shows | Screen, folder listing |
| B3 | Press Esc while converting (underline shown) | Only the conversion is cancelled; the field keeps its text; the dialog stays | Screen |
| B4 | Commit, then press Esc | The field closes (ordinary Esc) | Screen |
| B5 | Type full-width `ＡＢＣ`, a full-width space, symbols | Entered as typed; cursor right of the text | Screen |
| B6 | Type the supplementary-plane ideograph `𠮷` and `😀` | Shown as one character each; the name after `Done:` is the same | Screen, `dir` output |
| B7 | Backspace after Japanese text | Removes one character (a surrogate pair counts as one); never `?` | Screen |
| B8 | Move inside Japanese text with ← → Home End and insert | Never splits a character | Screen |
| B9 | Repeat the text-entry, conversion, cancellation and cursor checks from B1–B8 in filter (`/`), path (`g`), rename (F2) and the built-in editor (`e`); omit file creation outside its dialog | Input remains intact and Unicode boundaries are preserved | Each screen |
| B10 | IME off, type ASCII | As usual | Screen |
| B11 | **Where the composition appears**: press `e` to open the built-in editor (NANO preset), turn the IME on, type `つきがきれいですね`, press Space to convert and **stop without committing** | The uncommitted text (underlined) and the candidate list appear at the insertion point of the line being edited, not near the status line at the bottom | Screenshot of the whole window during composition (record the window size too) |
| B12 | Repeat B11 with the window maximized | Appears at the insertion point as at the normal size | Screenshot when maximized |
| B13 | After B11, press Enter to commit | The committed text lands where the composition was shown | Screen after committing |
| B14 | Repeat B11 in rename (F2), new file name (`a` then `%`), filter (`/`), Go (Ctrl+L), the Vim command line (`:`) after switching profiles, and a text field in Settings | In every field the composition appears at the field's insertion point | Each screen |
| B15 | **How the cursor looks**: during B11, do the terminal cursor and the reversed pseudo cursor look doubled? Is there no cursor on the file list (normal screen)? After `q`, is the shell cursor shown as usual? | While editing, one cursor at the insertion point (the reversal swapping with the blink is fine). No cursor on the list. Shown after exit | Screens while editing, on the list and after exit (record the cursor shape setting) |
| B16 | **A question with the IME on**: type Japanese in the NANO editor, press Ctrl+X (the "Save changes?  S Save  D Discard  C Cancel" question). Leave the IME in **hiragana mode** | The question shows with `Enter = C` at the end (the selection starts on Cancel). It can be answered **without typing S, D or C** | Screen with the question |
| B17 | In B16 press Up: `Enter = D`; Down: `Enter = S`; Down again: `Enter = C`. Press Enter alone | Up and Down change the selection. Enter on Cancel cancels (the edits stay, nothing is saved or discarded) | Screen at each selection |
| B18 | In B16, with the IME on, press `d` or `s` (the underlined composition appears), and commit a full-width `ｄ` | The question does not close. Nothing is saved or discarded. Enter (the IME's commit) leaves the question open or cancels it, nothing more | Screen, file contents |
| B19 | In B16 press Up then Enter (discard); on another run Down then Enter (save) | Discard leaves the file unchanged; save writes the Japanese text | File contents |

Automated coverage of committed text: the `edit_jp_*` checks in `PRODUCT-TUI`
and `tests/test_term_input.c`. They do not exercise IME conversion.

## C. Installer, PATH, signing, SmartScreen

Build `piero-Setup-x64.exe` with Inno Setup using the [packaging guide](packaging.md).
Record build and install/uninstall results separately; perform installation
only in a snapshot-backed disposable VM.

| # | Steps | Expected | Evidence on failure |
|---|-------|----------|---------------------|
| C1 | `python tools/check_package.py <payload> --executable piero.exe --installer-helper` | PASS (inventory and hashes match) | Full output |
| C2 | Unpack the ZIP into a new empty folder; `piero.exe --version` | Starts; `LICENSE`, `LICENSES\` and `languages\` sit beside it | Listing |
| C3 | (VM) Run the installer; PATH option unchecked by default | Installs under `%LOCALAPPDATA%\Programs`; no elevation prompt | Install folder, any UAC dialog |
| C4 | (VM) Uninstall C3, then install with PATH checked; new terminal: `piero --version` | Works; no duplicate PATH entry; registry value type preserved | `reg query HKCU\Environment /v Path` before and after |
| C5 | (VM) Another program changes PATH, then uninstall | PATH left untouched; manual review requested | PATH value, screen |
| C6 | (VM) Uninstall | Does not recursively delete personal data under `%APPDATA%\piero`, `%LOCALAPPDATA%\piero`, `%APPDATA%\wfile` or `%LOCALAPPDATA%\wfile` | Folder listings |
| C7 | `signtool verify /pa /v piero.exe` | No signing key is configured: unsigned today. Verified after signing | Full output |
| C8 | SmartScreen: download the ZIP in a browser, unpack, run (Mark-of-the-Web) | A warning may appear. Do not disable protection to get past it. Record the result | Warning screenshot |

## D. SSH/SFTP (read-only browsing)

See [features](features.md) for supported operations. Browsing and reading work; there is no writing. It uses your own `ssh.exe` and a host name from your own ssh configuration; piero never reads your keys or configuration and never accepts a host key. Use noninteractive SSH authentication; password and keyboard-interactive prompts are disabled. Only test a server you own and may use for this purpose. **NOT RUN** until performed.

| # | Steps | Expected | Evidence on failure |
|---|-------|----------|---------------------|
| D1 | In a terminal run `ssh <host>` once and accept the key yourself | The key is in `known_hosts` | Terminal |
| D2 | In piero press `g`, type `sftp:<host>/home` and Enter | The folder is listed; the status line says `SFTP (read-only)` | Screen |
| D3 | Do D2 with a host whose key is NOT known yet | A message says the host key is not known; no SFTP session opens and the key is not accepted | Screen |
| D4 | Press `a`, `R`, Delete, `y`, `x`, `p`, `o` there | Every one is refused with "SFTP places are read-only here" | Screen |
| D5 | Select a small text file and press `e` | It opens read-only; Ctrl+S is refused; "Save as" to a Windows folder works | Screen |
| D6 | Stop the server during a listing, then press `r` to reload | A reason is shown after a time limit (not a hang); a new try makes 1 new connection | Screen |

## E. WSL

Rows E1-E2 use the read-only research probe `probe_wsl`; E3 onward use the product. Do these only in a folder you made for the purpose (for example under `/tmp`), never in real data.

| # | Steps | Expected | Evidence on failure |
|---|-------|----------|---------------------|
| E1 | With a stopped distribution, run `probe_wsl` | Record the first-listing latency, including distribution startup; afterwards `wsl --terminate <name>` only for the dedicated test distribution | Full output |
| E2 | Read the result | FileId and case-sensitivity info are unavailable on this path; the product therefore uses its own WSL operations instead of the identity-bound engines | Full output |
| E3 | In Windows Terminal press `g`, type `wsl:` and Enter | A message "Type wsl:<distribution> (installed: ...)" lists the registered distributions; none is started | Screen; `wsl -l --running` before and after |
| E4 | `g`, `wsl:<distribution>` (a RUNNING one), Enter | The distribution's root is listed; the status line says `WSL` | Screen |
| E5 | In a scratch folder of your own (for example `/tmp/try`) press `a` (create menu) and `F2`/`R` (rename) | Files and folders can be created and renamed; an existing name is refused, never replaced; `o` (open with another program) is refused | Screen |
| E6 | In the distribution create `File` and `file` (ext4), then list | Both are shown; selecting and re-entering the folder returns to the right one | Screen |
| E7 | A symbolic link in the listing | Shown as a link; Enter does not follow it | Screen |
| E8 | Copy a file with `y` then `p` in another disposable folder with a free name; repeat with an occupied name; then `x` and `p` into a subfolder; try `x` then `p` in a Windows folder | A free name is copied; an occupied name is refused. Move works inside the distribution and is refused toward Windows | Screen and source/destination hashes |
| E9 | Select a file and a folder with content, press Delete | A permanent-delete question names the count; `n` or Esc deletes nothing, `y` deletes the item and folder with its content | Screen |
| E10 | Edit a script file with `e`, change 1 line, save with Ctrl+S | The Linux mode (`ls -l`) is unchanged; the old text is in `<name>~`; if the file is changed from Linux between opening and saving, the save is refused | `ls -l`, the screen |
