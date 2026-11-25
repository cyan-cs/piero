#ifndef WF_CANONICAL_WIN_H
#define WF_CANONICAL_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>

#include "base/wf_status.h"

typedef struct
{
	uint64_t volume;
	wchar_t *path; /* owned, normalized volume-GUID prefix, exact path tail
	                */
} WfCanonicalPath;

/* Only the GUID/prefix is folded. Invalid or ambiguous components refuse.
 * No filesystem authority is implied by success. Caller frees *out. */
bool wf_canonical_normalize(const wchar_t *path, wchar_t **out);
bool wf_canonical_from_handle(
        HANDLE handle, WfCanonicalPath *out, DWORD *error);
/* The same, also on a FAT volume (no FileId: the volume serial is used). Only
 * for work on a new name there; see wf_weakid_win.h. */
bool wf_canonical_from_handle_weak_ok(
        HANDLE handle, WfCanonicalPath *out, DWORD *error);
/* Whether two canonical paths are the same, and why not when they cannot be told:
 * WF_OK with *equal (false: they differ, which is an answer), or WF_E_INVAL when
 * one is not a path that can be compared. Nothing is allocated, so out of memory
 * is not a cause. */
WfStatus wf_canonical_compare(
        const WfCanonicalPath *a, const WfCanonicalPath *b, bool *equal);
/* The same as true only for "equal": different and not comparable are both
 * false. For a place that only refuses; a place that tells the person which it
 * was uses wf_canonical_compare. */
bool wf_canonical_equal(const WfCanonicalPath *a, const WfCanonicalPath *b);
void wf_canonical_free(WfCanonicalPath *path);

#endif
