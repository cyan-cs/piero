[English](safety.md) | [日本語](../ja/safety.md)

# Safety and limitations

piero is a development preview. Automated tests cover file identity, read-back
verification, journal recovery and refusal paths. Passing results apply to those
tested cases; they do not establish safety across every filesystem, device,
antivirus product, concurrent program, crash or power loss.

## File operations

Keep independent backups of important files.

Copy uses SHA-256 read-back verification by default. Explicit `verify=none` disables
that check. A cross-volume move verifies the copy before sending its source to the
Recycle Bin. A cancelled or failed job can leave completed items at the destination;
inspect the per-item results before retrying.

Overwrite permanently removes the previous destination file after replacement.
It cannot be restored through piero's Undo or the Recycle Bin. Copy merges existing
directories; move does not. Copies do not preserve the source's ACLs, extended
attributes, encryption or compression state. An overwrite retains the destination's
access permissions. Links, encrypted files and online-only sources are refused
or skipped. See [features](features.md) for supported operations.

Recycling uses a Shell operation by path. Identity checks before and after narrow
the race, but another process can replace a name between the last check and the
Shell operation. An uncertain result requires inspection. Use folders whose names
other processes cannot change during the operation.

FAT and exFAT volumes (many USB sticks) have no FileId, so piero identifies a
file there by the volume serial and the directory-entry index. That identity is
weaker: it can repeat after a delete, it changes when an entry is renamed, and
two cloned sticks can share a serial. It is used only for work that leaves what
exists as it is: browsing, opening a file, creating a new file or folder, and
copying files to or from the stick. Nothing there is moved, replaced, renamed
or deleted by piero, a folder is not copied onto it (it is built under a hidden
name and renamed), and Undo does not offer its new items. A copy still compares
the file after it is written.

Undo and history exist only for the current run. They do not undo overwrite,
Recycle Bin deletion or cross-volume moves. WSL deletion is permanent and uses a
separate confirmation; SFTP is read-only.

## Editor saves and backups

A replacing editor or configuration save retains the original under the first free
backup name: `name~`, then `name~2` through `name~9`. Saving is refused when all
9 names are occupied. Review backups with `B` before freeing a slot. Save As never
overwrites an existing name. Elevated saving is unavailable.

WSL editor saves first copy the old contents to a backup, then write directly to
the existing file to retain its Linux mode and owner. This is not an atomic
replacement: a crash or power loss during saving can leave partial contents.
On a reported write failure, piero attempts to restore the old contents; restoration
can also fail. Keep the backup and inspect both files before retrying.

Draft recovery is separate from backups. It can restore only text successfully
written before interruption, subject to size and filename exclusions. Filename
checks do not inspect content for secrets. See [configuration](configuration.md)
for draft and session persistence settings. Neither mechanism guarantees that the
last edit survives a crash or power loss.

## State folders and recovery

Configuration normally lives under `%APPDATA%\piero`; local records, drafts and
working files live under `%LOCALAPPDATA%\piero`. When only the older `wfile` folder
exists, it remains in use. When both exist, piero uses `piero` and checks `wfile`
for unresolved records. It does not migrate or remove the older folder.

Unknown, malformed or unresolved records can block copying and replacing saves.
Inspect Settings > About and the startup diagnostics. Keep the records,
backups, originals and leftover temporary files for inspection. Copy receipts in
`copy-journal.txt` are an inventory, not an automatic rollback or resume mechanism.
Bulk rename has its own recovery prompt at the next start.

`--abandon-recovery <id>` inspects a local recovery record and requires the exact
ID to be typed back. When permitted, it abandons local journal/metadata only;
it does not repair files or access the recorded target volume. Unknown ownership
is refused. This command does not clear copy receipts or bulk-rename journals.
`--purge-state` is unavailable. `--forget-session` clears saved session state only.
Do not delete the entire state folder to dismiss a warning.

## Preview isolation and verification

Images and PDF pages are decoded in AppContainer with memory and time limits.
If isolation cannot start, use text display; do not bypass the isolation checks.
Optional syntax DLLs execute native code with piero's privileges. Install only
matching trusted grammar packs; see [syntax colouring](syntax.md).

ConPTY and unit tests do not replace checks on real terminals, IMEs, storage,
installers or external servers. See [manual validation](manual-validation.md)
and [manual verification procedures](manual-verification-procedures.md).
