/* The "program" a test association starts: records where it was started
 * from and which file it was given, next to that file as "<file>.seen".
 * Line 1: the working directory. Line 2: the file argument. UTF-16, no BOM. */
#include <windows.h>

#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
	wchar_t text[2200], seen[1100];
	DWORD got, wrote;
	HANDLE h;

	if (argc < 2 || wcslen(argv[1]) > 1000) {
		return 2;
	}
	got = GetCurrentDirectoryW(1000, text);
	if (0 == got || got >= 1000) {
		return 3;
	}
	swprintf(seen, 1100, L"%ls.seen", argv[1]);
	swprintf(text + got, 1200, L"\n%ls\n", argv[1]);
	h = CreateFileW(seen, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	if (INVALID_HANDLE_VALUE == h) {
		return 4;
	}
	WriteFile(h, text, (DWORD)(wcslen(text) * sizeof *text), &wrote, NULL);
	CloseHandle(h);
	return 0;
}
