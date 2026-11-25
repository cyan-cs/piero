# P0 manual terminal smoke — procedure

This procedure covers the P0 terminal collectors and the current development
application. Use disposable local files and the Standard keyboard profile.
For IME, image/PDF previews, installers and remote locations, also follow the
[current manual verification guide](en/manual-verification-procedures.md)
([日本語](ja/manual-verification-procedures.md)).

Why manual: Windows Terminal key encoding, real key repeat, IME composition and
rendering cannot be exercised faithfully from a script. The ConPTY automation
(`p0_conpty_host`) covers piero's console layer only and is recorded as such.

## 0. Before you start

* From a configured testing build, run `cmake --build out/dev --target piero p0_terminal p0_env`.
  The build must contain `piero.exe`, `p0_terminal.exe` and `p0_env.exe`.
* Terminals to cover: Windows Terminal + PowerShell 7,
  Windows Terminal + PowerShell 5.1, Windows Terminal + cmd, conhost + cmd,
  conhost + PowerShell 5.1. Run one combination per run directory; combinations
  you do not run stay **NOT TESTED**.
* Start a run: `tools\p0\run_manual_smoke.ps1 -Build out/dev -TerminalHost wt -Shell pwsh`
  (or use `-Build out/dev -TerminalHost conhost -Shell cmd`). It opens three collector windows
  one after another and waits for each. The product smoke is performed separately
  with the dedicated configuration and state described in section 5.
* If Windows' default terminal is Windows Terminal, `conhost.exe` launched
  directly still gives a classic console window; confirm the window chrome.
* Record, in `result.json`: your keyboard (JIS/US), IME name and state.

## 1. Rules

* Record what you saw. If you did not do an item, leave it `NOT_TESTED`.
* Keep the collector logs with screenshots so observations can be checked.
* Do not edit the `expected` text.

## 2. Session 1 — raw key log (T01, T02)

Type, in order, pausing about a second between groups. End with **Shift+Q**.

1. `abc`, then the four arrow keys, `F2`, `Enter`, `Tab`, `Backspace`
2. `/` `?` `.` (JIS keyboard: use the real keys; try with IME OFF)
3. Hold `j` for about 2 seconds, release (real key repeat)
4. Switch the IME ON; type `ひらがな`, press `Space` to convert, `Enter` to commit
5. Switch IME OFF again; resize the window by dragging; press `Esc` once
6. Press `Ctrl+A`, then (in WT only) paste a line with an emoji (`😀`)

Afterwards `traces\input_log.csv` is the evidence. Things to look for are listed
in `result.json` items M13, M14.

## 3. Session 2 — modal guard (T05, T06, T02)

`p0_terminal --mode confirm-test` opens the real CONFIRM modal with `x`.

| # | Do | Expect | Item |
|---|----|--------|------|
| a | Hold `x` for 2 s, release | modal opens once; repeats never confirm | M15 |
| b | Press `x`, immediately mash `Y` | nothing for the first ~300 ms; later `y` fires once | M16 |
| c | Press `x`, press `Esc` at once | back to NORMAL immediately | M17 |
| d | `x`, wait 1 s, IME ON, type `は` | no confirmation; IME hint appears; `Esc` cancels | M18 |
| e | resize to 80x24, 60x20, 40x15, 40x12, 20x8, then 20x5 with the modal open | verb + destination visible; below 20x8 `CONFIRM: Y/N` | M21 |

Quit with `Ctrl+C`.

## 4. Session 3 — width matrix (T03, T04)

For every row the `resynced` marker `|` must sit in **column 41** (compare with
the ruler). Write down which rows make the `naive` marker drift — that is the
terminal's width disagreement with piero, which the CUP re-sync is meant to
contain. Check the half-block row for gaps (M20). Press `Q`.

## 5. Session 4 — piero smoke

Prepare a fixture using the [manual validation guide](en/manual-validation.md)
([日本語](ja/manual-validation.md)), then run its launcher in a real terminal
with the English UI and Standard profile. The collector script does not launch
piero or select a data directory for you. Check M01–M12 in `result.json`.
For M08, copy a synthetic file
into a different disposable folder, then compare the source and destination
SHA-256 hashes. For M09, attempt to copy it in the same folder; confirm that preflight refuses the
operation, the original is unchanged and no copy was made.
For M12 close the tab/window while piero is running and check Task Manager.
These tests exercise real file operations; the copy engine is enabled.

## 6. Finish

Fill `result.json`, then `tools\p0\run_manual_smoke.ps1 -Finalize <dir>`. The run
becomes PASS only if every item is PASS; any FAIL makes it FAIL; any
NOT_TESTED keeps it BLOCKED.
