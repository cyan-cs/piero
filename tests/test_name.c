#include "path/wf_name.h"
#include "wf_test.h"
#include "base/wf_macro.h"

/* "a/b/c" is a name with folders on the way: every part is a name of its own,
 * none is empty, "." or "..", and there are not too many. */
static void names_with_folders_on_the_way(void)
{
	const wchar_t *good[] = {
	        L"a", L"a/b", L"a\\b/c.txt", L"test/hello", L"漢字/😀"};
	const wchar_t *bad[] = {L"", L"/", L"/a", L"a/", L"a//b", L"a/./b",
	        L"a/../b", L"..", L"a/b.", L"a/NUL", L"a/b:c", L"C:/x", L"a/*"};
	wchar_t deep[2 * (WF_NAME_PATH_DEPTH + 1) + 1];
	const wchar_t *rest;
	size_t i, n;

	for (i = 0; i < WF_ARRAY_LEN(good); i++) {
		CHECK(WF_NAME_OK ==
		        wf_name_validate_path(good[i], wcslen(good[i]), 255));
	}
	for (i = 0; i < WF_ARRAY_LEN(bad); i++) {
		CHECK(WF_NAME_OK !=
		        wf_name_validate_path(bad[i], wcslen(bad[i]), 255));
	}
	/* the depth limit: WF_NAME_PATH_DEPTH parts pass, one more does not */
	for (i = 0; i < WF_NAME_PATH_DEPTH + 1; i++) {
		deep[2 * i] = L'a';
		deep[2 * i + 1] = L'/';
	}
	deep[2 * (WF_NAME_PATH_DEPTH + 1) - 1] = 0;
	CHECK(WF_NAME_OK ==
	        wf_name_validate_path(deep, 2 * WF_NAME_PATH_DEPTH - 1, 255));
	CHECK(WF_NAME_TOO_LONG ==
	        wf_name_validate_path(
	                deep, 2 * (WF_NAME_PATH_DEPTH + 1) - 1, 255));
	/* a component that is too long for the volume */
	CHECK(WF_NAME_TOO_LONG == wf_name_validate_path(L"a/bcd", 5, 2));

	n = wf_name_split_first(L"test/hello", &rest);
	CHECK(4 == n && rest && !wcscmp(rest, L"hello"));
	n = wf_name_split_first(L"a\\b/c", &rest);
	CHECK(1 == n && rest && !wcscmp(rest, L"b/c"));
	n = wf_name_split_first(L"hello", &rest);
	CHECK(5 == n && !rest);
}

int main(void)
{
	const wchar_t *bad[] = {L"", L".", L"..", L"a.", L"a ", L"a:b", L"x/y",
	        L"a\\b", L"<a", L"x>y", L"x\"y", L"a|b", L"a?b", L"*",
	        L"a\x001b!", L"\u202eexe.txt", L"a\u200bb", L"\xd800",
	        L"\xdc00"};
	const wchar_t *reserved[] = {L"NUL", L"nul.txt", L"NUL.tar.gz",
	        L"CON.foo", L"CONIN$", L"conout$.txt", L"PRN", L"aux",
	        L"COM1.log", L"lpt9.x", L"COM¹", L"LPT².a", L"COM³",
	        L"NUL .txt"};
	const wchar_t *good[] = {L"COM0", L"LPT0", L"ordinary.txt", L"漢字",
	        L"emoji\xd83d\xde00", L".gitignore", L"COM10"};
	wchar_t limit[257];
	size_t i;
	for (i = 0; i < WF_ARRAY_LEN(bad); i++) {
		CHECK(WF_NAME_OK !=
		        wf_name_validate(bad[i], wcslen(bad[i]), 255));
	}
	for (i = 0; i < WF_ARRAY_LEN(reserved); i++) {
		CHECK(WF_NAME_RESERVED ==
		        wf_name_validate(
		                reserved[i], wcslen(reserved[i]), 255));
	}
	for (i = 0; i < WF_ARRAY_LEN(good); i++) {
		CHECK(WF_NAME_OK ==
		        wf_name_validate(good[i], wcslen(good[i]), 255));
	}
	for (i = 0; i < 256; i++) {
		limit[i] = L'漢';
	}
	limit[256] = 0;
	CHECK(WF_NAME_OK == wf_name_validate(limit, 255, 255));
	CHECK(WF_NAME_TOO_LONG == wf_name_validate(limit, 256, 255));
	CHECK(WF_NAME_OK != wf_name_validate(L"a", 1, 0));
	CHECK(WF_NAME_OK != wf_name_validate(NULL, 1, 255));
	CHECK(WF_NAME_OK != wf_name_validate(L"a\0b", 3, 255));
	names_with_folders_on_the_way();
	return TEST_RESULT();
}
