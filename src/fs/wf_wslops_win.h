#ifndef WF_WSLOPS_WIN_H
#define WF_WSLOPS_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"

/*
 * Changes to files in a WSL distribution reached through \\wsl.localhost.
 * That path has no file identity, so none of the identity-bound engines can
 * run on it; these operations rely on what it does give: names that are
 * case-sensitive, atomic "create new", and "rename without replacing".
 *
 *  - Nothing is ever replaced: a name that exists is refused.
 *  - A link (reparse point) is never followed. Deleting one removes the link
 *    itself; copying and descending into one are refused.
 *  - A copy is built under a temporary name, compared byte for byte with its
 *    source, and only then given its name (without replacing).
 *  - Work is bounded (entries, depth, bytes); over the bound nothing starts.
 *
 * Not preserved by a copy: the Linux mode bits and owner of the source.
 * Deleting is permanent: a distribution has no Recycle Bin.
 */

#define WF_WSLOPS_MAX_ENTRIES 20000u
#define WF_WSLOPS_MAX_DEPTH 48u
#define WF_WSLOPS_MAX_BYTES ((unsigned long long)256 << 20)

typedef struct
{
	DWORD error;
	const char *reason;  /* NULL when none; a static string */
	size_t done;         /* entries finished before the stop */
	size_t warnings;     /* things that did not matter to the bytes: */
	const char *warning; /* e.g. a time that could not be kept */
} WfWslReport;

/* The one name of a new entry: no separators, no characters Windows would
 * rewrite, no trailing dot or space, not "." or "..". */
bool wf_wslops_name_ok(const wchar_t *name);

/* Whether folder `dst` is the folder src[0..n) or lies inside it: case is not told
 * apart in the \wsl.localhost\<distro> part and is told apart after it. */
bool wf_wslops_is_inside(const wchar_t *dst, const wchar_t *src, size_t n);

WfStatus wf_wslops_create(
        const wchar_t *dir, const wchar_t *name, bool folder, WfWslReport *r);

/* The same for a name with folders on the way ("a/b/c", see
 * wf_name_validate_path). */
WfStatus wf_wslops_create_path(
        const wchar_t *dir, const wchar_t *path, bool folder, WfWslReport *r);

/* Renames inside one folder, or moves between folders of the same share; the
 * target name must be free. */
WfStatus wf_wslops_rename(const wchar_t *from_dir,
        const wchar_t *from,
        const wchar_t *to_dir,
        const wchar_t *to,
        WfWslReport *r);

/* Permanently deletes `path`. A folder needs `tree`, and only then are its
 * contents removed (links inside are removed, not followed). */
WfStatus wf_wslops_delete(const wchar_t *path, bool tree, WfWslReport *r);

/* Copies the file or folder `src` (local or WSL) to `dst_dir`\`name`. */
WfStatus wf_wslops_copy(const wchar_t *src,
        const wchar_t *dst_dir,
        const wchar_t *name,
        WfWslReport *r);

/* Bytes copied since the last reset, for a progress display (any thread). */
void wf_wslops_progress_reset(void); /* also clears a cancel request */
/* Asks the running operation to stop at the next chunk or entry; whatever it
 * finished stays finished and is counted in `done`. Any thread. */
void wf_wslops_request_cancel(void);
unsigned long long wf_wslops_progress(void);

/* Removes `dir`\`name` only if it is an empty file or an empty folder (the
 * undo of a create must not take away what was put there since). */
WfStatus wf_wslops_remove_if_empty(
        const wchar_t *dir, const wchar_t *name, WfWslReport *r);

/* The same, naming the items as folder + leaf. */
WfStatus wf_wslops_copy_in(const wchar_t *src_dir,
        const wchar_t *name,
        const wchar_t *dst_dir,
        const wchar_t *dst_name,
        WfWslReport *r);
WfStatus wf_wslops_delete_in(
        const wchar_t *dir, const wchar_t *name, bool tree, WfWslReport *r);

#endif
