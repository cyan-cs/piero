# piero_e2e_ui

A small Windows helper for real-machine E2E runs that need a real window: list, look at,
screenshot and send keys to a terminal window that the run itself created. It is a tool for
the test machine, not part of the product build, and is not in CMake.

## Build (clang-cl, from the repository root)

```text
clang-cl /std:c17 /W4 /WX /utf-8 /permissive- /O2 /Fo<outdir>\ /Fe<outdir>\piero_e2e_ui.exe ^
  tools\e2e\piero_e2e_ui.c /link user32.lib gdi32.lib ole32.lib oleaut32.lib uuid.lib ^
  dwmapi.lib /DEPENDENTLOADFLAG:0x800
```

## What it cannot do

No shell, no child process, no registry, no network, no file reads, no elevation. It writes
only PNG files, only to `%TEMP%\piero-e2e-*\<name>.png`, and never replaces a file.

## Policy

- Read commands (`list`, `state`, `uia`, `shot`) work on windows of `WindowsTerminal.exe`,
  `conhost.exe` and `piero.exe`. Titles of other windows are printed as `<hidden>`.
- Commands that change something (`focus`, `key`, `hold`, `unicode`, `resize`, `close`) also
  need `piero-e2e-` in the window title. Start the window with
  `wt -w new new-tab --title piero-e2e-<id> --suppressApplicationTitle ...` and find it
  with `wait piero-e2e-<id> <ms>`.
- Keys are sent only while the target is the foreground window, checked before every send;
  the run stops with exit 3 otherwise. The Windows keys cannot be named. At most 64 chords,
  5 s of `hold`, 30 s per run.
- `shot ... print` (default) reads the window itself. `shot ... rect` copies the screen and
  is refused (`occluded`) unless the window is topmost at nine points.
- `unicode` injects characters directly and prints `ime=bypassed`. It is not IME
  composition and is never IME verification.
- `uia` reads names, control types, ids, classes and rectangles; never a value or text.

Exit codes: 0 ok, 1 usage, 2 refused by policy, 3 state missing or lost, 4 OS error,
124 deadline.

## Not verified

The race between the foreground check and `SendInput` is small but not zero. Only one
terminal host (Windows Terminal 1.24, cmd inside) was exercised. `piero.exe` and
`conhost.exe` windows are in the allowlist but were not tried.
