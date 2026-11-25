#ifndef WF_RUN_WIN_H
#define WF_RUN_WIN_H
#include <windows.h>

typedef enum
{
	WF_RUN_EXITED,
	WF_RUN_START_FAILED,
	WF_RUN_WAIT_FAILED,
	WF_RUN_TIMED_OUT,
	WF_RUN_TERMINATE_FAILED,
	WF_RUN_DESCENDANTS,
	WF_RUN_CRASHED
} WfRunState;
typedef struct
{
	WfRunState state;
	DWORD error;
	LONG exit_code;
} WfRunResult;

/* Test tools only. command is a writable CreateProcess command line.
 * The job is assigned while suspended; only three explicit stdio handles are
 * inherited. Timeout never means a successful test, even after a confirmed kill. */
WfRunResult wf_run_process(const wchar_t *exe,
        wchar_t *command,
        const wchar_t *out,
        const wchar_t *err,
        DWORD timeout_ms);
#endif
