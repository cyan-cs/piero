#ifndef WF_PROCESS_CONTROL_WIN_H
#define WF_PROCESS_CONTROL_WIN_H

#include <windows.h>

#include "wf_lock.h"

/*
 * Inspection, close requests and termination for processes selected in the
 * file-lock UI (DR-DRAFT-024). These actions run on the lock worker's thread;
 * the UI does not call the process-control APIs directly.
 *
 * A process is named by PID **and start time**. Every action opens the process
 * itself, checks its own creation time against the start time it was given,
 * gathers the facts for wf_lock_decide while keeping that handle open.
 * Termination uses the same handle. Close requests post WM_CLOSE to visible
 * windows after checking their owner PID; the process handle prevents PID
 * reuse but does not pin a window against destruction/reuse. Privileges are
 * never raised, and a fact that cannot be read refuses the action.
 * Missing ancestry records, a cycle or the ancestry-depth limit leave ancestry
 * unknown and refuse the action, even if the target's identity is readable.
 */

typedef struct
{
	uint32_t self_pid; /* 0: this process (tests may name another) */
} WfProcEnv;

/* The process list as it was at one moment: the parent of each process. One
 * query shares this snapshot across the holders it describes. An action does
 * not reuse that query snapshot: it reads a fresh parent list and checks
 * process facts through its own handle. Zero it before the first use;
 * wf_proc_snap_free after the last. */
typedef struct
{
	DWORD *ids, *parents;
	size_t n;
	unsigned reads; /* how many times the list was read into this */
	bool read, ok;  /* read at all, and completely */
} WfProcSnap;

void wf_proc_snap_free(WfProcSnap *s);

/* Fills the identity, image, session, window and policy fields of `h`, whose
 * pid, start, app name and type were set by the caller (from Restart Manager).
 * Never fails: what cannot be read stays empty and refuses both actions.
 * `snap` may be NULL: the process list is then read for this one. */
void wf_proc_describe_with(
        WfLockHolder *h, const WfProcEnv *env, WfProcSnap *snap);
void wf_proc_describe(WfLockHolder *h, const WfProcEnv *env);

typedef struct
{
	WfLockActionResult result;
	WfLockDeny deny;     /* result DENIED: why */
	unsigned long error; /* result FAILED: the Win32 code */
} WfProcAction;

/* Posts WM_CLOSE to the visible top-level windows of the process and waits up
 * to `wait_ms` for it to exit (also ends early when `cancel` is signalled; it
 * may be NULL). EXITED only when the process was seen to exit; STILL when it
 * was not observed to exit (timeout, cancellation or failed wait). A process
 * may still be asking about unsaved data. Does not call TerminateProcess. */
WfProcAction wf_proc_request_close(uint32_t pid,
        uint64_t start,
        WfLockAppType type,
        DWORD wait_ms,
        HANDLE cancel,
        const WfProcEnv *env);

/* TerminateProcess on the verified handle, then waits up to `wait_ms` and
 * reports EXITED only if the process was seen to exit. */
WfProcAction wf_proc_end(uint32_t pid,
        uint64_t start,
        WfLockAppType type,
        DWORD wait_ms,
        const WfProcEnv *env);

/* FILETIME as the 64-bit tick count. */
uint64_t wf_proc_ft64(FILETIME ft);

#endif
