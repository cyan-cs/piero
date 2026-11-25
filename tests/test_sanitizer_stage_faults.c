/* Isolated API faults in the same staging implementation. No product hooks. */
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "../tools/p0/p0_lab.h"
#include "wf_test.h"

static wchar_t fake_runtime[1024];
static bool use_fake, short_read, failed_write, short_write;

static DWORD module_path(HMODULE module, LPWSTR path, DWORD cap)
{
	if (!use_fake) {
		return GetModuleFileNameW(module, path, cap);
	}
	size_t n = wcslen(fake_runtime);
	if (n >= cap) {
		return cap;
	}
	memcpy(path, fake_runtime, (n + 1) * sizeof *path);
	return (DWORD)n;
}
static BOOL read_bytes(
        HANDLE h, LPVOID p, DWORD n, LPDWORD got, LPOVERLAPPED ov)
{
	BOOL ok = ReadFile(h, p, n, got, ov);
	if (short_read && ok && *got) {
		(*got)--;
		SetLastError(0);
	}
	return ok;
}
static BOOL write_bytes(
        HANDLE h, LPCVOID p, DWORD n, LPDWORD put, LPOVERLAPPED ov)
{
	if (failed_write) {
		SetLastError(ERROR_DISK_FULL);
		return FALSE;
	}
	BOOL ok = WriteFile(h, p, short_write && n ? n - 1 : n, put, ov);
	if (short_write) {
		SetLastError(0);
	}
	return ok;
}
#define GetModuleFileNameW module_path
#define ReadFile read_bytes
#define WriteFile write_bytes
#include "../src/preview/wf_sanitizer_stage_win.c"
#undef GetModuleFileNameW
#undef ReadFile
#undef WriteFile

int main(void)
{
	P0Lab lab = {0};
	wchar_t path[1024];
	DWORD error = 0, written = 0;
	CHECK(p0_lab_open(&lab, "san-runtime-faults", 0));
	if (!lab.area) {
		return TEST_RESULT();
	}
	CHECK(p0_lab_path(&lab, L"foreign.dll", fake_runtime, 1024));
	HANDLE h = CreateFileW(
	        fake_runtime, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(h != INVALID_HANDLE_VALUE);
	if (h != INVALID_HANDLE_VALUE) {
		CHECK(WriteFile(h, "FOREIGN", 7, &written, NULL) &&
		        written == 7);
		CloseHandle(h);
	}
	for (int scenario = 0; scenario < 4; scenario++) {
		wchar_t rel[32];
		swprintf(rel, 32, L"case-%d", scenario);
		CHECK(p0_lab_path(&lab, rel, path, 1024));
		CHECK(CreateDirectoryW(path, NULL));
		use_fake = scenario == 0;
		short_read = scenario == 1;
		failed_write = scenario == 2;
		short_write = scenario == 3;
		CHECK(!wf_sanitizer_stage(path, &error));
		DWORD expected[] = {ERROR_INVALID_DATA, ERROR_READ_FAULT,
		        ERROR_DISK_FULL, ERROR_WRITE_FAULT};
		CHECK(error == expected[scenario]);
		use_fake = short_read = failed_write = short_write = false;
		if (scenario >= 2) {
			/* Existing incomplete files must never be repaired by truncation. */
			CHECK(!wf_sanitizer_stage(path, &error));
			CHECK(error == ERROR_INVALID_DATA);
		}
	}
	CHECK(p0_lab_close(&lab, 1) == 0);
	return TEST_RESULT();
}
