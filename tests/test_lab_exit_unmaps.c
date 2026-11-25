/* A test process that leaves through exit() (the mutation harness does so on
 * the first failing check) must not leave its drive letter mapped: the
 * mapping is machine-wide, and 23 leaked letters stop every later test that
 * needs a private drive.
 *
 * The parent starts this program again as a child; the child opens a lab with
 * a drive, prints the letter and exits at once without closing the lab. The
 * parent then asks the system whether that letter is still defined. */
#include <windows.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static int child(void)
{
	P0Lab lab;

	if (!p0_lab_open(&lab, "lab-exit", 1)) {
		printf("open failed: %lu\n", lab.error);
		return 3;
	}
	printf("drive=%lc\n", lab.drive[0]);
	fflush(stdout);
	exit(1); /* the lab is deliberately not closed */
}

static bool run_child(const wchar_t *self, wchar_t *letter)
{
	wchar_t cmd[1100];
	SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
	HANDLE rd = NULL, wr = NULL;
	STARTUPINFOW si = {.cb = sizeof si};
	PROCESS_INFORMATION pi;
	char out[256] = "";
	DWORD got = 0, code = 0;
	const char *at;
	bool ok = false;

	if (!CreatePipe(&rd, &wr, &sa, 0) ||
	        !SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0)) {
		return false;
	}
	swprintf(cmd, 1100, L"\"%ls\" --child", self);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = si.hStdError = wr;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	if (CreateProcessW(
	            NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
		CloseHandle(wr);
		wr = NULL;
		WaitForSingleObject(pi.hProcess, 60000);
		GetExitCodeProcess(pi.hProcess, &code);
		ReadFile(rd, out, sizeof out - 1, &got, NULL);
		out[got] = '\0';
		CloseHandle(pi.hProcess);
		CloseHandle(pi.hThread);
		at = strstr(out, "drive=");
		if (1 == code && at && at[6] >= 'D' && at[6] <= 'Z') {
			*letter = (wchar_t)at[6];
			ok = true;
		} else {
			printf("child: code=%lu output=[%s]\n", code, out);
		}
	}
	if (wr) {
		CloseHandle(wr);
	}
	CloseHandle(rd);
	return ok;
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t self[1100], letter = 0, drive[3] = L"?:", target[1024];

	if (2 == argc && !wcscmp(argv[1], L"--child")) {
		return child();
	}
	GetModuleFileNameW(NULL, self, 1100);
	CHECK(run_child(self, &letter));
	if (letter) {
		drive[0] = letter;
		/* still defined means it leaked */
		CHECK(!QueryDosDeviceW(drive, target, 1024));
		if (QueryDosDeviceW(drive, target, 1024)) {
			/* take back only the mapping this test's child made, so a
			 * failing run does not poison the machine for the next one */
			DefineDosDeviceW(DDD_REMOVE_DEFINITION |
			                DDD_EXACT_MATCH_ON_REMOVE |
			                DDD_RAW_TARGET_PATH |
			                DDD_NO_BROADCAST_SYSTEM,
			        drive, target);
		}
	}
	return TEST_RESULT();
}
