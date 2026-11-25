#ifndef WF_GITPROC_WIN_H
#define WF_GITPROC_WIN_H

#include <windows.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../base/wf_status.h"

/*
 * Runs subprocesses for the Windows and WSL Git backends and captures their
 * output (DR-DRAFT-038).
 *
 *  - No shell: `exe` is a full path and `args` an array; each argument is quoted
 *    as needed for CreateProcessW (wf_terminal_quote_arg), preserving argument
 *    boundaries. The caller is responsible for option semantics.
 *  - The child has the null device as its input and two pipes as its output and
 *    error, and nothing else of piero's (a handle list), no console window.
 *  - The child joins a KILL_ON_JOB_CLOSE job before it is resumed. Cancellation
 *    and timeout request job termination and wait up to 2 seconds for the child;
 *    closing the job requests termination of remaining processes.
 *  - Output is bounded: past `max_out` bytes the child is ended and the answer
 *    is WF_E_RANGE. Only the first bytes of the error output are kept.
 *  - The pipe-draining loop checks `cancelled` at 20 ms intervals; true requests
 *    termination (WF_E_CANCELLED). `timeout_ms` returns WF_E_TIMEOUT; 0 disables it.
 *  - The environment is piero's, without the variables that point Git
 *    elsewhere (GIT_DIR, GIT_WORK_TREE, GIT_INDEX_FILE, GIT_COMMON_DIR,
 *    GIT_NAMESPACE) and with the ones in `extra_env` ("NAME=value").
 */
typedef struct
{
	const wchar_t *exe; /* full path */
	const wchar_t *const
	        *args;      /* after the program name; NULL-terminated */
	const wchar_t *cwd; /* NULL: piero's */
	const wchar_t *const *extra_env; /* NULL or NULL-terminated */
	uint32_t timeout_ms;
	size_t max_out; /* stdout byte limit; 0: no configured limit */
	bool (*cancelled)(void *ctx);
	void *cancel_ctx;
} WfGitRun;

typedef struct
{
	int exit_code; /* meaningful when the status is WF_OK */
	uint8_t *
	        out; /* malloc'd, NUL-terminated after out_n; NULL when empty */
	size_t out_n;
	char err[256]; /* the start of the error output, NUL-terminated */
	/* Some of the error output is not in `err`: it was over what is kept, or
	 * there was no memory to keep it. It is a diagnostic and never fails the
	 * run; the program's own output (`out`) is another matter, and an output
	 * that is too big (WF_E_RANGE) or cannot be held (WF_E_NOMEM) fails it. */
	bool err_dropped;
	DWORD os_error; /* captured Win32 setup/start error when available */
} WfGitRunResult;

/* WF_OK: the program ran and ended (see exit_code). WF_E_NOENT: the program is
 * not there; WF_E_TIMEOUT, WF_E_CANCELLED, WF_E_RANGE as above; WF_E_OS and
 * WF_E_NOMEM otherwise. Valid arguments initialise the result even on failure;
 * free its output with wf_gitproc_result_free. Invalid arguments return
 * WF_E_INVAL before result initialization. */
WfStatus wf_gitproc_run(const WfGitRun *run, WfGitRunResult *res);
void wf_gitproc_result_free(WfGitRunResult *res);

#endif
