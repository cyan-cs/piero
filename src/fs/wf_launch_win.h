#ifndef WF_LAUNCH_WIN_H
#define WF_LAUNCH_WIN_H

#include <windows.h>

#include "wf_launch.h"

/*
 * Opens one file through ShellExecuteExW with the default verb. Before the
 * call the file is opened for attributes only, without FILE_SHARE_DELETE, and
 * kept open until the shell has returned: that proves the object is the one
 * the listing showed (same FILE_ID_INFO, a plain file, not a reparse point)
 * and stops it being renamed or replaced during the call. Readers and
 * writers are not blocked, so the handler can open the file normally.
 *
 * `error` receives GetLastError() or the SE_ERR_* code. Never elevates (no
 * "runas"); never changes the process directory.
 */
WfStatus wf_launch_checked(const wchar_t *path,
        const WfFileId *expected,
        const wchar_t *directory,
        DWORD *error);

#endif
