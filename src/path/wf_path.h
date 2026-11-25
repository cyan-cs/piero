#ifndef WF_PATH_H
#define WF_PATH_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

#include "../base/wf_status.h"

/*
 * Paths are NUL-terminated UTF-16 (wchar_t) strings. Functions that return
 * `wchar_t *` return malloc'd memory owned by the caller (free()).
 *
 * These are pure string operations: they never touch the filesystem and do
 * not canonicalize. Callers pass absolute, already-normalized paths (e.g.
 * from GetFullPathNameW). Anything that would need normalization to be safe
 * is rejected rather than guessed at.
 */

/* Length of the parent of `p`, or 0 if `p` is a root (or has no parent).
 * The parent of "C:\a" is "C:\" (the root keeps its backslash). */
size_t wf_path_parent_len(const wchar_t *p);

/* Last component of `p`, ignoring trailing backslashes. Borrowed from p.
 * Empty (len 0) for a root. */
void wf_path_leaf(const wchar_t *p, const wchar_t **leaf, size_t *len);

/*
 * "This PC": the place above every drive. Its path is a name Windows never
 * resolves (as the SFTP root is), so a path function handed it by mistake
 * reaches nothing. Its entries are the drives ("C:") and the WSL
 * distributions ("wsl:Ubuntu"); wf_path_join gives their real paths ("C:\",
 * "\\wsl.localhost\Ubuntu") and makes nothing else from a name here.
 */
#define WF_PCROOT L"\\\\pc.invalid\\computer"
bool wf_pcroot_is(const wchar_t *p);
/* True for "\\<host>.invalid\...": stand-in paths for "This PC" and SFTP.
 * Keep them out of Win32 opens, watches and probes: resolving a stand-in
 * host can block the UI while name lookup times out. */
bool wf_path_host_invalid(const wchar_t *p);
/* When `p` is the root of a drive or of a WSL distribution: the name of that
 * entry in "This PC" (malloc'd), so "go to the parent" can select it. NULL
 * for any other path. */
wchar_t *wf_pcroot_leaf(const wchar_t *p);

/* dir + '\' + name. `name` must be a single component (no '\' or '/'). */
WfStatus wf_path_join(const wchar_t *dir, const wchar_t *name, wchar_t **out);

/* Returns a copy of the first `n` units of `p`; NULL is allowed only for n = 0. */
wchar_t *wf_path_dup_n(const wchar_t *p, size_t n);
/* A copy of the whole string `p` (NULL for NULL, or when memory is short). */
wchar_t *wf_path_dup(const wchar_t *p);

/*
 * Win32 extended form for long paths: "C:\x" -> "\\?\C:\x",
 * "\\srv\sh\x" -> "\\?\UNC\srv\sh\x". Already-extended input is copied.
 * WF_E_INVAL for relative paths, '/' separators, or "." / ".." components,
 * because the \\?\ prefix turns off the normalization that would resolve
 * them.
 */
WfStatus wf_path_to_extended(const wchar_t *abs, wchar_t **out);

#endif
