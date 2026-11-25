#ifndef WF_EDIT_WIN_H
#define WF_EDIT_WIN_H
#include <windows.h>

#include "../path/wf_name.h"
#include "../xfer/wf_xfer.h"
/* `renamed` is true once the filesystem accepted a rename/move, even if a later
 * post-check fails: the status then says "failed" but the object has moved. */
typedef struct
{
	DWORD error;
	WfNameResult name;
	bool renamed;
} WfEditReport;
/* Candidate backend for P0-N01. No overwrite, deletion, normalization or merge.
 * These two unchecked entry points are isolated PoC/test APIs only: they cannot
 * prove the parent is the directory observed by a product caller. */
WfStatus wf_edit_create(const wchar_t *parent,
        const wchar_t *name,
        int directory,
        WfEditReport *report);
WfStatus wf_edit_rename(const wchar_t *parent,
        const wchar_t *old_name,
        const WfFileId *expected,
        const wchar_t *new_name,
        WfEditReport *report);
/* Product-connection candidates: require the observed parent's full identity.
 * Compare after pinning the parent and before any child mutation; missing,
 * invalid or stale IDs fail closed. Still subject to independent gates. */
WfStatus wf_edit_create_checked(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *name,
        int directory,
        WfEditReport *report);
/* The same for a name with folders on the way ("a/b/c", see
 * wf_name_validate_path): missing folders are made, existing ones are entered. */
WfStatus wf_edit_create_path_checked(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *path,
        int directory,
        WfEditReport *report);
WfStatus wf_edit_rename_checked(const wchar_t *parent,
        const WfFileId *expected_parent,
        const wchar_t *old_name,
        const WfFileId *expected,
        const wchar_t *new_name,
        WfEditReport *report);
/* True when every component of the absolute `path` is a real folder (none is a
 * reparse point). *error is the Win32 error when a component cannot be read. */
bool wf_edit_path_chain_ok(const wchar_t *path, unsigned long *error);
/* Candidate only: local same-volume, handle-based, no replacement or copy
 * fallback. All three identities are mandatory. Failure after mutation is not
 * rollback. */
WfStatus wf_edit_move(const wchar_t *source_parent,
        const WfFileId *expected_source_parent,
        const wchar_t *old_name,
        const WfFileId *expected_source,
        const wchar_t *destination_parent,
        const WfFileId *expected_destination_parent,
        const wchar_t *new_name,
        WfEditReport *report);
#endif
