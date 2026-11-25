[English](../en/architecture.md) | [日本語](../ja/architecture.md)

# Architecture

## Boundaries

piero is ISO C17 with Windows UTF-16 and ABI assumptions, not a portable library.
Win32 names are UTF-16; display is sanitized UTF-8. `src/main.c` starts services;
`src/main_pump.c` coordinates input, worker events, directory notifications and
deadlines. The application modules under `src/ui/` own view state and generations;
`src/render/wf_present.c` presents text cell differences.
Sixel has a separate graphics layer and explicit erase/presentation rules.

## Ownership and concurrency

Directory snapshots own arrays, raw names and display buffers. Views own cursor,
filter and scroll state. Requests copy borrowed strings; poll transfers owned
results. Directory/text/image/preflight observations are replaceable; stale
generations are rejected. Accepted file-operation receipts cannot be silently superseded.
Navigation Memory is a position hint, independent of filesystem authorization.
The prefetch window is bounded; failed speculative requests are not retried endlessly.

Directory notifications distinguish changes in the current listing, the selected
child and unrelated descendants. Overflow or malformed notifications cause a
conservative refresh. A preview retained while revalidating is explicitly a
previous read-only view, with identity and metadata checks; it never authorizes
file operations. This avoids discarding and rereading an unchanged preview for
unrelated activity. Regression coverage lives in `tests/test_preview_stability.c`
and `tests/test_watch_notifications.c`.

`stop` consumes ownership even on timeout. Deferred workers retain their own
buffers until I/O returns and transfer a completion handle where specified.
Callers close it and keep borrowed contexts alive until completion. UI destruction
may cancel requests, so the service ports remain alive until that destruction finishes.
Permanently stuck OS I/O cannot be fixed by freeing a live worker's memory.

## Storage and isolation

`fs/` contains checked filesystem operations and ownership proofs; `xfer/`
contains policy, copy/verification, receipts and workers; `edit/` handles text
and checked file replacement. `recovery/` handles owned local storage records,
which are distinct from the copy engine's manual inventory.
`preview/` bounds text and image input and runs WIC/Windows.Data.Pdf inside
AppContainer. `archive/` uses statically linked read-only libarchive/decompressors.
`config/`, `input/` and `i18n/` hold configuration, stable commands/keymaps and translations.

## Navigation limits

Navigation Memory retains positions for 128 folders. Listings hold up to 500000
entries and folder previews up to 2000. Prefetch reads up to 5 folders ahead and
3 behind within an 8 MiB budget, without following links. These bounds describe
resource use, not a guarantee of responsiveness on every filesystem.

## Implementation references

Use [features](features.md), [safety](safety.md) and [development](development.md)
for current behavior and constraints. The [documentation entry point](../README.md)
links to guide maintenance and test procedures.

The implementation lives under `src/`; regression tests live under `tests/`.
Check both when changing ownership, cancellation, recovery or operation gates.
