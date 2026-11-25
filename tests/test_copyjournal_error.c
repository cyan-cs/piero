/* Fault injection: cleanup may change thread-local Win32 last-error state. */
#include <windows.h>
#include <stdlib.h>
#include "wf_test.h"

static DWORD injected_open_error, cleanup_error;

static HANDLE failing_open(LPCWSTR path,
        DWORD access,
        DWORD share,
        LPSECURITY_ATTRIBUTES security,
        DWORD creation,
        DWORD attributes,
        HANDLE template_file)
{
	(void)path;
	(void)access;
	(void)share;
	(void)security;
	(void)creation;
	(void)attributes;
	(void)template_file;
	SetLastError(injected_open_error);
	return INVALID_HANDLE_VALUE;
}

static void clobbering_free(void *memory)
{
	free(memory);
	SetLastError(cleanup_error);
}

#define CreateFileW failing_open
#define free clobbering_free
#include "../src/xfer/wf_copyjournal_win.c"
#undef free
#undef CreateFileW

int main(void)
{
	const DWORD errors[] = {ERROR_FILE_NOT_FOUND, ERROR_ACCESS_DENIED,
	        ERROR_PATH_NOT_FOUND, ERROR_SHARING_VIOLATION};
	for (size_t i = 0; i < sizeof errors / sizeof errors[0]; i++) {
		WfCopyJournalInfo result;
		injected_open_error = errors[i];
		cleanup_error = injected_open_error == ERROR_FILE_NOT_FOUND
		        ? ERROR_ACCESS_DENIED
		        : ERROR_FILE_NOT_FOUND;
		CHECK(wf_cj_inspect(L"C:\\not-accessed\\journal", &result) ==
		        (injected_open_error == ERROR_FILE_NOT_FOUND));
		CHECK(result.records == 0);
	}
	return TEST_RESULT();
}
