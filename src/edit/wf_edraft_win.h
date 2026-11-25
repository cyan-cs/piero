#ifndef WF_EDRAFT_WIN_H
#define WF_EDRAFT_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>

#include "../base/wf_status.h"
#include "wf_eenc.h"

/*
 * Best-effort recovery of unsaved editor text. Each edited path has two slot
 * files under %LOCALAPPDATA%\<state>\drafts, where <state> is selected by
 * wf_state_classify (piero or legacy wfile). Names come from a hash of the
 * path; each write uses the older or incomplete slot, keeping the other one.
 * The user's own folders are not touched.
 *
 * A draft holds the user's text in plain form outside the file's own folder,
 * so wf_draft_allowed excludes names associated with secrets, selected private
 * folders, UNC paths, and text larger than 2 MiB. This is a name-based filter;
 * it does not inspect the text for secrets. The folder inherits the per-user permissions of
 * %LOCALAPPDATA% and is marked not to be content-indexed. A restored draft
 * remains available until the buffer is saved or its changes are discarded.
 */

/* False for a path whose text must never be kept as a draft, or a text too
 * large to be one. */
bool wf_draft_allowed(const wchar_t *path, size_t bytes);

/* Replaces the draft of `path` with this text (UTF-8, as the editor holds it)
 * and its encoding. */
WfStatus wf_draft_write(
        const wchar_t *path, WfEncoding enc, const char *utf8, size_t n);

/* The draft of `path`: WF_E_NOENT when there is none. *utf8 is malloc'd.
 * Outputs are required; a null or empty path returns WF_E_INVAL. */
WfStatus wf_draft_read(
        const wchar_t *path, WfEncoding *enc, char **utf8, size_t *n);

/* Removes the draft of `path` (none, null or empty is not an error). */
void wf_draft_remove(const wchar_t *path);

/* Tests only: keep drafts in this folder instead of under LOCALAPPDATA. */
void wf_draft_set_dir(const wchar_t *dir);

#endif
