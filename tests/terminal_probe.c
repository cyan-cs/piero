/*
 * Stands in for wt.exe in the tests of Open Terminal Here: it writes the
 * working directory it was started in and its arguments (UTF-16, one per line)
 * to the file named by WF_TERMINAL_PROBE_OUT, and exits. No window is opened.
 */
#include <windows.h>

#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

int main(void)
{
	wchar_t out[1100], cwd[MAX_PATH + 8];
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	FILE *f;

	if (!GetEnvironmentVariableW(L"WF_TERMINAL_PROBE_OUT", out, 1100) ||
	        !argv) {
		return 2;
	}
	if (!GetCurrentDirectoryW(MAX_PATH + 8, cwd)) {
		return 3;
	}
	if (0 != _wfopen_s(&f, out, L"wb") || !f) {
		return 4;
	}
	fwrite(cwd, sizeof(wchar_t), wcslen(cwd), f);
	for (int i = 1; i < argc; i++) {
		fwrite(L"\n", sizeof(wchar_t), 1, f);
		fwrite(argv[i], sizeof(wchar_t), wcslen(argv[i]), f);
	}
	fclose(f);
	LocalFree(argv);
	return 0;
}
