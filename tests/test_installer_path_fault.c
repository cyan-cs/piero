/* Exercises refusal paths without opening or changing the registry. */
#include <windows.h>
#include <stdlib.h>

#include "path_entry.h"
#include "wf_test.h"

static int expansion_failure;
static bool comparison_failure;
static size_t attempts, fail_at;

static void *path_malloc(size_t n)
{
	return ++attempts == fail_at ? NULL : malloc(n);
}

static DWORD path_expand(LPCWSTR input, LPWSTR output, DWORD capacity)
{
	if (!capacity && expansion_failure == 1) {
		return 0;
	}
	if (!capacity && expansion_failure == 2) {
		return 32769;
	}
	if (capacity && expansion_failure == 3) {
		return 0;
	}
	if (capacity && expansion_failure == 4) {
		return capacity + 1;
	}
	return ExpandEnvironmentStringsW(input, output, capacity);
}

static int path_compare(LPCWSTR a, int an, LPCWSTR b, int bn, BOOL ignore_case)
{
	return comparison_failure
	        ? 0
	        : CompareStringOrdinal(a, an, b, bn, ignore_case);
}

#define malloc path_malloc
#define ExpandEnvironmentStringsW path_expand
#define CompareStringOrdinal path_compare
#ifndef WF_PATH_ENTRY_SOURCE
#	define WF_PATH_ENTRY_SOURCE "../tools/path_entry.c"
#endif
#include WF_PATH_ENTRY_SOURCE
#undef CompareStringOrdinal
#undef ExpandEnvironmentStringsW
#undef malloc

int main(void)
{
	const wchar_t *dir = L"C:\\Apps\\piero";
	const wchar_t *other = L"C:\\Apps\\other";
	wchar_t *appended;

	CHECK(!piero_path_contains(other, dir));
	appended = piero_path_append(other, dir);
	CHECK(appended &&
	        !wcscmp(appended, L"C:\\Apps\\other;C:\\Apps\\piero"));
	free(appended);
	for (int failure = 1; failure <= 4; failure++) {
		expansion_failure = failure;
		CHECK(piero_path_contains(other, dir));
		CHECK(!piero_path_append(other, dir));
	}
	expansion_failure = 0;
	comparison_failure = true;
	CHECK(piero_path_contains(other, dir));
	CHECK(!piero_path_append(other, dir));
	comparison_failure = false;
	for (size_t failure = 1; failure <= 2; failure++) {
		attempts = 0;
		fail_at = failure;
		CHECK(piero_path_contains(other, dir));
		attempts = 0;
		CHECK(!piero_path_append(other, dir));
	}
	return TEST_RESULT();
}
