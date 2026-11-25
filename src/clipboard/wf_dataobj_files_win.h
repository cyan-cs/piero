#ifndef WF_DATAOBJ_FILES_WIN_H
#define WF_DATAOBJ_FILES_WIN_H

#include <windows.h>

#include <objidl.h>

#include "../base/wf_status.h"

/*
 * The data object piero puts on the clipboard so that Explorer (and any Shell
 * compatible program) can paste files from it: CF_HDROP and "Preferred
 * DropEffect" (copy), as an IDataObject written in C with its own reference count.
 *
 * It holds the names and nothing else. The files are not opened, read or held by
 * it, and nothing a receiver asks of it (including SetData of the "Performed
 * DropEffect" and "Paste Succeeded" formats a receiver writes back after a move)
 * makes piero do anything to a file: piero never deletes or moves on the word of
 * the clipboard. A receiver asks for each format and gets a fresh copy.
 */

/* `*out` has one reference, for the caller. WF_E_RANGE: a name this program
 * would not read back, or too many. */
WfStatus wf_dataobj_files_create(
        const wchar_t *const *path, size_t n, IDataObject **out);

/* How many of these objects exist now (for tests: it must return to zero). */
LONG wf_dataobj_files_live(void);

#endif
