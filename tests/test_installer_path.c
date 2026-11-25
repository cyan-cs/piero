#include "path_entry.h"
#include "wf_test.h"
#include <stdlib.h>
#include <string.h>
int main(void)
{
	const wchar_t *dir = L"C:\\Apps\\piero";
	CHECK(piero_path_contains(L"C:\\Other;\"c:\\apps\\PIERO\\\";", dir));
	CHECK(!piero_path_contains(L"C:\\Apps\\piero-old", dir));
	wchar_t *a = piero_path_append(L"C:\\Other;", dir);
	CHECK(a && !wcscmp(a, L"C:\\Other;C:\\Apps\\piero"));
	CHECK(a && piero_path_can_restore(a, a, L"C:\\Other;", dir));
	CHECK(a &&
	        !piero_path_can_restore(L"C:\\Changed", a, L"C:\\Other;", dir));
	CHECK(a && !piero_path_can_restore(a, a, L"C:\\Wrong", dir));
	CHECK(!piero_path_append(dir, dir));
	CHECK(!piero_path_append(L"", L"C:\\Bad;Name"));
	free(a);
	return TEST_RESULT();
}
