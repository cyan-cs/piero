/* A notice read must complete before it is displayed as the license text. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "ui/wf_app_settings_priv.h"
#include "wf_test.h"

typedef enum
{
	NOTICE_OK,
	NOTICE_FAILED,
	NOTICE_SHORT
} NoticeFault;
static NoticeFault notice_fault;

static BOOL WINAPI notice_read(
        HANDLE file, LPVOID out, DWORD n, LPDWORD got, LPOVERLAPPED at)
{
	if (notice_fault == NOTICE_FAILED) {
		*got = 0;
		SetLastError(ERROR_CRC);
		return FALSE;
	}
	return ReadFile(file, out,
	        notice_fault == NOTICE_SHORT && n ? n - 1 : n, got, at);
}

#define ReadFile notice_read
#ifndef WF_ABOUT_SOURCE
#	define WF_ABOUT_SOURCE "../src/ui/wf_app_about.c"
#endif
#include WF_ABOUT_SOURCE
#undef ReadFile

int main(void)
{
	static WfApp app;
	static const char body[] = "Copyright \xEF\xBF\xBD\n";
	wchar_t temp[MAX_PATH], dir[MAX_PATH + 80], file[MAX_PATH + 100];
	DWORD length = GetTempPathW(WF_ARRAY_LEN(temp), temp), written = 0;
	HANDLE h;
	int count;

	if (!length || length >= WF_ARRAY_LEN(temp)) {
		CHECK_FAIL("cannot locate temporary directory");
		return TEST_RESULT();
	}
	count = swprintf(dir, WF_ARRAY_LEN(dir), L"%lswf-notice-%lu-%llu", temp,
	        (unsigned long)GetCurrentProcessId(),
	        (unsigned long long)GetTickCount64());
	if (count <= 0 || (size_t)count >= WF_ARRAY_LEN(dir) ||
	        !CreateDirectoryW(dir, NULL)) {
		CHECK_FAIL("cannot create an owned notice fixture");
		return TEST_RESULT();
	}
	swprintf(file, WF_ARRAY_LEN(file), L"%ls\\notice.txt", dir);
	h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		CHECK_FAIL("cannot create the notice");
		RemoveDirectoryW(dir);
		return TEST_RESULT();
	}
	CHECK(WriteFile(h, body, sizeof body - 1, &written, NULL));
	CHECK(written == sizeof body - 1);
	CloseHandle(h);
	app.license_dir = dir;
	app.settings.lic_n = 1;
	strcpy_s(app.settings.lic_name[0], sizeof app.settings.lic_name[0],
	        "notice");
	for (int mode = NOTICE_OK; mode <= NOTICE_SHORT; ++mode) {
		char *text;
		size_t n = 0;
		notice_fault = (NoticeFault)mode;
		text = wf_about_text(&app, WF_SEC_LICENSES, 1, &n);
		const char *expected =
		        mode == NOTICE_OK ? body : wf_msg(MSG_LIC_MISSING);
		CHECK(text && n == strlen(expected) &&
		        !memcmp(text, expected, n));
		free(text);
	}
	CHECK(DeleteFileW(file));
	CHECK(RemoveDirectoryW(dir));
	return TEST_RESULT();
}
