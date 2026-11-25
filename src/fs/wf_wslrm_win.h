#ifndef WF_WSLRM_WIN_H
#define WF_WSLRM_WIN_H

#include <windows.h>

#include <stdbool.h>

/*
 * Path-based removal for WSL (\\wsl.localhost) work: a distribution has no file identity, so
 * the ownership proof owned_delete is built on cannot exist there. What stands
 * in for it is the callers' own rules (see wf_wslops_win.h): the user was asked
 * and named the item, a link is removed as a link and never followed, and a
 * temporary or backup is only ever removed by the call that created it with
 * CREATE_NEW. tools/ci_check_banned_apis.ps1 allowlists this boundary.
 *
 * `ext` is an extended-length path (wf_path_to_extended).
 */

/* Removes the entry itself: a file, an (empty) folder, or a link without
 * touching what it points to. `attrs` is what the caller read for the entry.
 * On failure *error is the Win32 code. */
bool wf_wslrm_entry(const wchar_t *ext, DWORD attrs, DWORD *error);

/* Removes a file this very call created (CREATE_NEW) and no longer holds. */
bool wf_wslrm_created_file(const wchar_t *ext);

#endif
