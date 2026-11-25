/* Stop at mutex creation after checking the derived installation directory.
 * Neither the host registry nor a real named mutex is accessed. */
#include <windows.h>
#include <stdlib.h>
#include <wchar.h>
#include <string.h>

#include "path_entry.h"
#include "wf_test.h"

static const wchar_t *module_path, *expected_directory;
static wchar_t *module_buffer;
static unsigned mutex_calls;

static DWORD test_module_name(HMODULE module, LPWSTR out, DWORD capacity)
{
	size_t n = wcslen(module_path);

	(void)module;
	CHECK(n < capacity);
	if (n >= capacity) {
		return capacity;
	}
	memcpy(out, module_path, (n + 1) * sizeof *out);
	module_buffer = out;
	return (DWORD)n;
}

static HANDLE test_mutex(
        LPSECURITY_ATTRIBUTES attributes, BOOL owner, LPCWSTR name)
{
	(void)attributes;
	(void)owner;
	(void)name;
	mutex_calls++;
	CHECK(module_buffer && !wcscmp(module_buffer, expected_directory));
	return NULL;
}

#define GetModuleFileNameW test_module_name
#define CreateMutexW test_mutex
#define wmain installer_test_main
#ifndef WF_INSTALLER_SOURCE
#	define WF_INSTALLER_SOURCE "../tools/installer_path.c"
#endif
#include WF_INSTALLER_SOURCE
#undef wmain
#undef CreateMutexW
#undef GetModuleFileNameW

int main(void)
{
	static const wchar_t *const cases[][2] = {
	        {L"C:\\Apps\\piero\\piero-path.exe", L"C:\\Apps\\piero"},
	        {L"C:\\piero-path.exe", L"C:\\"},
	        {L"\\\\?\\C:\\piero-path.exe", L"\\\\?\\C:\\"},
	        {L"\\\\server\\share\\piero-path.exe", L"\\\\server\\share"}};
	wchar_t *args[] = {L"piero-path.exe", L"--add"};

	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		module_path = cases[i][0];
		expected_directory = cases[i][1];
		module_buffer = NULL;
		mutex_calls = 0;
		CHECK(installer_test_main(2, args) == 1);
		CHECK(mutex_calls == 1);
	}
	return TEST_RESULT();
}
