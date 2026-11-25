/* Read-only proof that pre-main imports selected the declared private runtime. */
#include <windows.h>
#include <stdbool.h>
#include <string.h>

#include "wf_test.h"

static bool identity(const wchar_t *path, FILE_ID_INFO *id)
{
	HANDLE file = CreateFileW(path, FILE_READ_ATTRIBUTES,
	        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	BOOL ok =
	        GetFileInformationByHandleEx(file, FileIdInfo, id, sizeof *id);
	CloseHandle(file);
	return ok != FALSE;
}

int wmain(int argc, wchar_t **argv)
{
	wchar_t path[1024];
	FILE_ID_INFO actual = {0}, expected = {0};
	HMODULE runtime = GetModuleHandleW(L"clang_rt.asan_dynamic-x86_64.dll");
	CHECK(argc == 2 && runtime != NULL);
	if (argc != 2 || !runtime) {
		return TEST_RESULT();
	}
	DWORD n = GetModuleFileNameW(runtime, path, 1024);
	CHECK(n != 0 && n < 1024);
	CHECK(GetProcAddress(runtime, "__asan_init") != NULL);
	if (n && n < 1024) {
		CHECK(identity(path, &actual));
		CHECK(identity(argv[1], &expected));
		CHECK(actual.VolumeSerialNumber == expected.VolumeSerialNumber);
		CHECK(!memcmp(actual.FileId.Identifier,
		        expected.FileId.Identifier,
		        sizeof actual.FileId.Identifier));
	}
	return TEST_RESULT();
}
