/* Read-only policy facts for a child this test owns; never controls a user app. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#include "wf_test.h"
#include "../src/process/wf_process_control_win.c"

int wmain(int argc, wchar_t **argv)
{
	if (3 == argc && !wcscmp(argv[1], L"--child")) {
		HANDLE release = (HANDLE)(uintptr_t)wcstoull(argv[2], NULL, 16);
		DWORD result = WaitForSingleObject(release, 10000);

		CloseHandle(release);
		return WAIT_OBJECT_0 == result ? 0 : 2;
	}
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE release = CreateEventW(&sa, TRUE, FALSE, NULL);
	wchar_t exe[32768], command[32880];
	STARTUPINFOW startup = {0};
	PROCESS_INFORMATION process = {0};
	FILETIME c, e, k, u;
	WfProcSnap snapshot = {0};
	Inspected in = {0};

	CHECK(release);
	startup.cb = sizeof startup;
	CHECK(GetModuleFileNameW(NULL, exe, 32768));
	swprintf(command, 32880, L"\"%ls\" --child %llx", exe,
	        (unsigned long long)(uintptr_t)release);
	CHECK(CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW,
	        NULL, NULL, &startup, &process));
	if (process.hProcess) {
		CHECK(GetProcessTimes(process.hProcess, &c, &e, &k, &u));
		in.h = process.hProcess;
		in.f.pid = process.dwProcessId;
		gather(&in, wf_proc_ft64(c), WF_LOCKAPP_UNKNOWN, true, NULL,
		        &snapshot);
		WfProcFacts *f = &in.f;
		bool ancestry_known = false;
		BOOL critical;
		DWORD session;
		PROCESS_PROTECTION_LEVEL_INFORMATION protection;
		DWORD critical_error =
		        IsProcessCritical(in.h, &critical) ? 0 : GetLastError();
		DWORD session_error = ProcessIdToSessionId(in.f.pid, &session)
		        ? 0
		        : GetLastError();
		DWORD protection_error =
		        GetProcessInformation(in.h, ProcessProtectionLevelInfo,
		                &protection, sizeof protection)
		        ? 0
		        : GetLastError();

		(void)is_ancestor(&snapshot, GetCurrentProcessId(), f->pid,
		        &ancestry_known);
		printf("process-facts: ancestry_known=%d critical_error=%lu "
		       "session_error=%lu protection_error=%lu\n",
		        ancestry_known, critical_error, session_error,
		        protection_error);

		printf("process-facts: session=%lu facts=%d identity=%d "
		       "user_known=%d same_user=%d ancestor=%d critical=%d "
		       "protected=%d system_image=%d terminate=%d window=%d "
		       "snapshot=%d close=%d end=%d\n",
		        (unsigned long)f->session, f->facts_ok, f->identity_ok,
		        f->user_known, f->same_user, f->ancestor, f->critical,
		        f->is_protected, f->system_image, f->can_terminate,
		        f->has_window, snapshot.ok, wf_lock_decide(f, false),
		        wf_lock_decide(f, true));
		CHECK(f->identity_ok && f->user_known && f->same_user);
		/* Missing ancestry or a non-interactive session must still refuse. */
		if (!f->facts_ok || !f->session) {
			CHECK(WF_DENY_NONE != wf_lock_decide(f, true));
		}
		SetEvent(release);
		CHECK(WAIT_OBJECT_0 ==
		        WaitForSingleObject(process.hProcess, 5000));
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
	}
	wf_proc_snap_free(&snapshot);
	CloseHandle(release);
	return TEST_RESULT();
}
