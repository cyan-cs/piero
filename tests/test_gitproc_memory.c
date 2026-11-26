/*
 * What the program's output costs in memory, apart from how much it is: an
 * output that cannot be held is out of memory (WF_E_NOMEM), not "too big"
 * (WF_E_RANGE); and an error output that cannot be kept is dropped and read on,
 * the run going on and saying so (err_dropped). The reallocation that grows the
 * output is made to fail, by its size, after the program has started.
 */
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wf_test.h"

static bool fail_growth;
static int fail_start;
static HANDLE observed_child;

static BOOL test_assign(HANDLE job, HANDLE process)
{
	if (fail_start) {
		CHECK(DuplicateHandle(GetCurrentProcess(), process,
		        GetCurrentProcess(), &observed_child, SYNCHRONIZE,
		        FALSE, 0));
	}
	if (1 == fail_start) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return AssignProcessToJobObject(job, process);
}

static DWORD test_resume(HANDLE thread)
{
	if (2 == fail_start) {
		SetLastError(ERROR_ACCESS_DENIED);
		return (DWORD)-1;
	}
	return ResumeThread(thread);
}

static BOOL test_exit_code(HANDLE process, LPDWORD code)
{
	if (3 == fail_start) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return GetExitCodeProcess(process, code);
}

static void *test_realloc(void *p, size_t n)
{
	if (fail_growth &&
	        4096 == n) { /* the first growth of a pipe's buffer */
		return NULL;
	}
	return realloc(p, n);
}

#define realloc test_realloc
#define AssignProcessToJobObject test_assign
#define ResumeThread test_resume
#define GetExitCodeProcess test_exit_code
#include "../src/git/wf_gitproc_win.c"
#undef realloc
#undef AssignProcessToJobObject
#undef ResumeThread
#undef GetExitCodeProcess

static WfStatus run_cmd(
        const wchar_t *script, size_t max_out, WfGitRunResult *r)
{
	wchar_t exe[MAX_PATH + 32], sys[MAX_PATH];
	const wchar_t *args[] = {L"/c", script, NULL};
	WfGitRun run;

	GetSystemDirectoryW(sys, MAX_PATH);
	swprintf(exe, MAX_PATH + 32, L"%ls\\cmd.exe", sys);
	memset(&run, 0, sizeof run);
	run.exe = exe;
	run.args = args;
	run.timeout_ms = 20000;
	run.max_out = max_out;
	return wf_gitproc_run(&run, r);
}

int main(void)
{
	WfGitRunResult r;
	DWORD before, after;

	/* CreatePipe/CreateProcess initialize process-wide Windows caches.
	 * A standalone API probe retains a pipe File and threadpool/ETW handles
	 * on first use; subsequent calls are stable. Establish that boundary
	 * with a successful run, then require exact balance on every fault. */
	CHECK(WF_OK == run_cmd(L"exit 0", 0, &r));
	wf_gitproc_result_free(&r);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (fail_start = 1; fail_start <= 3; fail_start++) {
		CHECK(WF_E_OS == run_cmd(L"echo owned-child", 0, &r));
		CHECK(ERROR_ACCESS_DENIED == r.os_error);
		CHECK(!r.out && !r.out_n);
		CHECK(observed_child &&
		        WAIT_OBJECT_0 ==
		                WaitForSingleObject(observed_child, 0));
		CloseHandle(observed_child);
		observed_child = NULL;
		wf_gitproc_result_free(&r);
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
		printf("git start fault=%d handles=%lu -> %lu\n", fail_start,
		        before, after);
		CHECK(before == after);
	}
	fail_start = 0;

	/* the output cannot be held: out of memory, and nothing is returned */
	fail_growth = true;
	CHECK(WF_E_NOMEM == run_cmd(L"echo hello", 0, &r));
	CHECK(!r.out && 0 == r.out_n);
	wf_gitproc_result_free(&r);
	/* the same with a limit that the output is far under: still not "range" */
	CHECK(WF_E_NOMEM == run_cmd(L"echo hello", 1 << 20, &r));
	wf_gitproc_result_free(&r);
	/* over the limit is still said as that */
	fail_growth = false;
	CHECK(WF_E_RANGE == run_cmd(L"echo 0123456789abcdef", 8, &r));
	wf_gitproc_result_free(&r);
	/* the error output that cannot be kept is dropped; the run is whole */
	fail_growth = true;
	CHECK(WF_OK == run_cmd(L"echo to-stderr 1>&2", 0, &r));
	CHECK(r.err_dropped && !r.err[0] && 0 == r.out_n);
	wf_gitproc_result_free(&r);
	/* and with memory, it is kept and nothing is said to be dropped */
	fail_growth = false;
	CHECK(WF_OK == run_cmd(L"echo hello&echo oops 1>&2", 0, &r));
	CHECK(!r.err_dropped && NULL != strstr(r.err, "oops") && r.out_n >= 5 &&
	        !memcmp(r.out, "hello", 5));
	wf_gitproc_result_free(&r);
	return TEST_RESULT();
}
