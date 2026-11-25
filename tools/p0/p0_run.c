#include "wf_run_win.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int main(void)
{
	int argc;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	wchar_t command[32768];
	WfRunResult result;
	HANDLE file;
	char json[160];
	DWORD written;
	unsigned long timeout;
	wchar_t *end;
	if (!argv || 6 != argc) {
		fputs("p0_run timeout_ms exe stdout stderr result.json\n",
		        stderr);
		if (argv) {
			LocalFree(argv);
		}
		return 2;
	}
	timeout = wcstoul(argv[1], &end, 10);
	if (*end || !timeout || timeout >= INFINITE ||
	        wcslen(argv[2]) >= 32765) {
		LocalFree(argv);
		return 2;
	}
	swprintf(command, 32768, L"\"%ls\"", argv[2]);
	result = wf_run_process(
	        argv[2], command, argv[3], argv[4], (DWORD)timeout);
	file = CreateFileW(argv[5], GENERIC_WRITE, FILE_SHARE_READ, NULL,
	        CREATE_NEW, 0, NULL);
	if (INVALID_HANDLE_VALUE == file) {
		LocalFree(argv);
		return 2;
	}
	int n = snprintf(json, sizeof json,
	        "{\"state\":%u,\"error\":%lu,\"exit_code\":%ld}\n",
	        (unsigned)result.state, result.error, result.exit_code);
	BOOL saved = n > 0 && (size_t)n < sizeof json &&
	        WriteFile(file, json, (DWORD)n, &written, NULL) &&
	        written == (DWORD)n;
	BOOL closed = CloseHandle(file);
	LocalFree(argv);
	return saved && closed
	        ? 0
	        : 2; /* tool success means a receipt, never a mutant kill */
}
