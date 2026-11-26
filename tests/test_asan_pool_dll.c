#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "wf_test.h"

typedef unsigned (*ReadGlobal)(unsigned index);

int main(int argc, char **argv)
{
	wchar_t path[32768];
	(void)argv;
	CHECK(GetModuleFileNameW(NULL, path, 32768));
	wchar_t *name = wcsrchr(path, L'\\');
	CHECK(name);
	if (!name) {
		return 1;
	}
	CHECK(0 ==
	        wcscpy_s(name + 1, 32768 - (size_t)(name + 1 - path),
	                L"asan_pool_dll.dll"));
	for (unsigned cycle = 0; cycle < 16; cycle++) {
		HMODULE module = LoadLibraryExW(path, NULL,
		        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
		                LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		CHECK(module);
		if (!module) {
			return 1;
		}
		FARPROC symbol = GetProcAddress(module, "wf_asan_pool_read");
		ReadGlobal read = NULL;
		_Static_assert(sizeof read == sizeof symbol,
		        "Win32 function pointers");
		memcpy(&read, &symbol, sizeof read);
		CHECK(read);
		if (read) {
			CHECK(0 == read(argc > 1 && 15 == cycle ? 1u : 0u));
		}
		CHECK(FreeLibrary(module));
		CHECK(NULL == GetModuleHandleW(path));
	}
	return TEST_RESULT();
}
