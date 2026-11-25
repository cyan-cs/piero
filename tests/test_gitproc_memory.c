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

static void *test_realloc(void *p, size_t n)
{
	if (fail_growth && 4096 == n) { /* the first growth of a pipe's buffer */
		return NULL;
	}
	return realloc(p, n);
}

#define realloc test_realloc
#include "../src/git/wf_gitproc_win.c"
#undef realloc

static WfStatus run_cmd(const wchar_t *script, size_t max_out, WfGitRunResult *r)
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
