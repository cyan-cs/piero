[English](../en/troubleshooting.md) | [日本語](../ja/troubleshooting.md)

# Troubleshooting

If image/PDF preview cannot start, keep AppContainer enabled and use text
preview. A failure to create the profile is different from a failure to start the isolated
process or decode a file. Do not run as administrator, grant broad ACLs or turn isolation off to hide
the error. Developers can compare the diagnostic probes in a normal user session.

A replacing save uses the first free backup name from `name~` to `name~9`.
If all 9 names are occupied, move backups somewhere safe or choose Save As. A recovery notice means you should look at the files that were kept; removing the receipt does
not repair an interrupted operation. See [limitations](safety.md).

Use text graphics when Sixel negotiation fails. Resize/restart may restore a
terminal after a crash; the terminal-reset command is documented in getting
started. Include terminal version, font, DPI and minimal steps in display reports.

An unavailable update check may mean offline access, rate limiting, a private
repository or no published release. Startup still continues. Downloads and
installation remain manual; see [updates](updates.md).

## Open is unavailable, or the copy is refused at startup

piero keeps a private working folder (`cwd`) and its logs inside its state
folder (`%LOCALAPPDATA%\piero`, or the older `wfile`). The folder must belong to
your account, with no inherited rights, and must not be a link. When that cannot
be verified, **Open and the logs stay off**, and Enter on a file says "OPEN
unavailable: piero's private state folder could not be verified". The line printed
before the screen opens names the cause:

| Printed cause | Meaning |
|---|---|
| not private to your account (owner or permissions) | the folder was made by something else (an installer, a restore from a backup, an elevated program, a copy) and has inherited or foreign rights |
| its working folder is not empty | something left a file in `cwd`; piero never deletes it |
| it is a link / not a folder | a junction, a symbolic link or a file stands where the folder should be |
| access to it is denied | your account may not open it |

piero never changes the permissions or the content of the folder to get past
this. Preserve the diagnostic and existing state, then review the named folder's
owner, permissions and contents. Do not rename or replace the whole state folder
to get past a recovery warning: a fresh folder can hide unresolved records.
If you cannot determine which folder failed the check, report the problem before
changing it. A folder created successfully by piero uses private permissions.

Copying is refused with its own reason: "the folder for piero's records cannot be
used" (the state folder is a link or file, or cannot be found), "the copy record
cannot be opened for writing" (access to the folder or the record file is
denied), or "unverified history" (see Settings > About). In every case nothing is
copied and no file is changed.

## File-lock queries or process actions are unavailable

Restart Manager can fail before it lists any programs. Keep the reported Windows
error code when reporting a failed query; an empty list does not prove a file is
unlocked. Close and End also require a verified process identity and ancestry.
If the process list is incomplete or ancestry cannot be established, piero refuses
the action even when the program is listed.

## Damaged session data

A damaged-session warning means that saved navigation state could not be restored.
It does not describe an operation-recovery record or an editor draft.
Session checkpoints refuse to replace data detected as damaged, unreadable or
unsupported, even when saving is enabled without `--continue`.
Preserve `session.a` and `session.b` and include the warning in a bug report.
If you no longer need that saved state, `piero --forget-session` empties both
files and allows new checkpoints. A failed clear keeps saving blocked.
Do not clear operation-recovery records to address a session warning.
