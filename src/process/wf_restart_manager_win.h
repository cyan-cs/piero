#ifndef WF_RESTART_MANAGER_WIN_H
#define WF_RESTART_MANAGER_WIN_H

#include <windows.h>

#include "wf_process_control_win.h"

/*
 * Which programs use one file, by the Restart Manager (DR-DRAFT-024):
 * RmStartSession, RmRegisterResources, RmGetList, and RmEndSession on every
 * path out. Nothing is shut down (RmShutdown is not used) and the file is not
 * opened.
 *
 * Results:
 *   WF_OK       `out` holds what RM named; it may be empty, which does NOT mean
 *               that nothing has the file open (drivers, network clients and
 *               programs this account may not inspect are not listed).
 *   WF_E_INVAL  not a file on a drive (a folder, a network or virtual path)
 *   WF_E_RANGE  a path RM cannot take (MAX_PATH units or more)
 *   WF_E_NOENT  the file is not there any more
 *   WF_E_NOMEM  out of memory
 *   WF_E_OS     RM or the system said no; *error is the Win32 code
 *
 * Each program is described with wf_proc_describe (identity, image, verdicts).
 */
WfStatus wf_rm_query(const wchar_t *path,
        const WfProcEnv *env,
        WfLockList *out,
        unsigned long *error);

/* For tests: Restart Manager sessions started and not yet ended by this
 * process (must be 0 whenever no query is running). */
long wf_rm_open_sessions(void);

#endif
