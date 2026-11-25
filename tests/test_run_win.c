#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static bool refuse_termination, fail_wait;
static BOOL terminate_job(HANDLE job, UINT code)
{
	if (refuse_termination) {
		refuse_termination = false;
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return TerminateJobObject(job, code);
}
static DWORD wait_process(HANDLE process, DWORD timeout)
{
	if (fail_wait) {
		fail_wait = false;
		SetLastError(ERROR_INVALID_HANDLE);
		return WAIT_FAILED;
	}
	return WaitForSingleObject(process, timeout);
}
#define TerminateJobObject terminate_job
#define WaitForSingleObject wait_process
#include "../tools/p0/wf_run_win.c"
#undef WaitForSingleObject
#undef TerminateJobObject

static wchar_t self[1024];
static P0Lab lab;
static unsigned sequence;
static WfRunResult run(const wchar_t *mode, DWORD timeout, bool missing)
{
	wchar_t out[1200], err[1200], command[1400], rel[64];
	swprintf(rel, 64, L"stdout-%u.txt", sequence);
	CHECK(p0_lab_path(&lab, rel, out, 1200));
	swprintf(rel, 64, L"stderr-%u.txt", sequence);
	CHECK(p0_lab_path(&lab, rel, err, 1200));
	swprintf(command, 1400, L"\"%ls\" %ls", self, mode);
	WfRunResult result = wf_run_process(
	        missing ? L"C:\\piero-nonexistent-test-executable.exe" : self,
	        command, out, err, timeout);
	swprintf(rel, 64, L"stdout-%u.txt", sequence);
	CHECK(p0_area_track(lab.area, rel, 0));
	swprintf(rel, 64, L"stderr-%u.txt", sequence++);
	CHECK(p0_area_track(lab.area, rel, 0));
	return result;
}

int main(int argc, char **argv)
{
	CHECK(GetModuleFileNameW(NULL, self, 1024) > 0);
	if (2 == argc) {
		if (!strcmp(argv[1], "hang")) {
			Sleep(INFINITE);
			return 2;
		}
		if (!strcmp(argv[1], "fail")) {
			puts("FAIL deliberate child assertion");
			return 1;
		}
		if (!strcmp(argv[1], "crash")) {
			ExitProcess(
			        0xC0000005u); /* deterministic abnormal status, no WER dialog */
		}
		if (!strcmp(argv[1], "descendant")) {
			STARTUPINFOW si = {0};
			PROCESS_INFORMATION pi = {0};
			wchar_t command[1200];
			si.cb = sizeof si;
			swprintf(command, 1200, L"\"%ls\" hang", self);
			if (!CreateProcessW(self, command, NULL, NULL, FALSE,
			            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
				return 1;
			}
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
		return 0;
	}
	CHECK(p0_lab_open(&lab, "process-supervisor", 0));
	if (!lab.area) {
		return 1;
	}
	DWORD before, after;
	/* Warm every loader path, including a failed CreateProcess, before the
	 * handle-retention check. Windows lazily opens process-wide handles. */
	for (unsigned round = 0; round < 2; round++) {
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
		WfRunResult r = run(L"ok", 5000, false);
		CHECK(WF_RUN_EXITED == r.state && 0 == r.exit_code);
		r = run(L"fail", 5000, false);
		CHECK(WF_RUN_EXITED == r.state && 1 == r.exit_code);
		r = run(L"crash", 5000, false);
		CHECK(WF_RUN_CRASHED == r.state &&
		        r.exit_code == (LONG)0xC0000005u);
		r = run(L"hang", 150, false);
		CHECK(WF_RUN_TIMED_OUT == r.state);
		r = run(L"ok", 5000, true);
		CHECK(WF_RUN_START_FAILED == r.state &&
		        ERROR_FILE_NOT_FOUND == r.error);
		fail_wait = true;
		r = run(L"hang", 5000, false);
		CHECK(WF_RUN_WAIT_FAILED == r.state &&
		        ERROR_INVALID_HANDLE == r.error);
		refuse_termination = true;
		r = run(L"hang", 150, false);
		CHECK(WF_RUN_TERMINATE_FAILED == r.state &&
		        ERROR_ACCESS_DENIED == r.error);
		r = run(L"descendant", 5000, false);
		CHECK(WF_RUN_DESCENDANTS == r.state);
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
		if (round) {
			CHECK(before == after);
		}
	}
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
