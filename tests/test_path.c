#include <stdlib.h>
#include <string.h>

#include "path/wf_path.h"
#include "wf_test.h"

static int parent_is(const wchar_t *p, const wchar_t *want)
{
	size_t n = wf_path_parent_len(p);

	return n == wcslen(want) && 0 == wcsncmp(p, want, n);
}

static int ext_is(const wchar_t *p, const wchar_t *want)
{
	wchar_t *e = NULL;
	int ok = WF_OK == wf_path_to_extended(p, &e) && 0 == wcscmp(e, want);

	free(e);
	return ok;
}

static int ext_rejected(const wchar_t *p)
{
	wchar_t *e = NULL;
	WfStatus st = wf_path_to_extended(p, &e);

	free(e);
	return WF_E_INVAL == st;
}

static void test_parent(void)
{
	CHECK(parent_is(L"C:\\a\\b", L"C:\\a"));
	CHECK(parent_is(L"C:\\a\\b\\", L"C:\\a"));
	CHECK(parent_is(L"C:\\a", L"C:\\"));
	CHECK(0 == wf_path_parent_len(L"C:\\"));
	CHECK(parent_is(L"\\\\srv\\share\\x", L"\\\\srv\\share\\"));
	CHECK(0 == wf_path_parent_len(L"\\\\srv\\share\\"));
	CHECK(0 == wf_path_parent_len(L"\\\\srv\\share"));
	CHECK(parent_is(L"\\\\?\\C:\\a\\b", L"\\\\?\\C:\\a"));
	CHECK(0 == wf_path_parent_len(L"\\\\?\\C:\\"));
	CHECK(parent_is(
	        L"\\\\?\\UNC\\srv\\share\\x", L"\\\\?\\UNC\\srv\\share\\"));
	CHECK(0 == wf_path_parent_len(L""));
}

static void test_leaf(void)
{
	const wchar_t *l;
	size_t n;

	wf_path_leaf(L"C:\\a\\bc", &l, &n);
	CHECK(2 == n && 0 == wcsncmp(l, L"bc", 2));
	wf_path_leaf(L"C:\\a\\bc\\\\", &l, &n);
	CHECK(2 == n && 0 == wcsncmp(l, L"bc", 2));
	wf_path_leaf(L"C:\\", &l, &n);
	CHECK(0 == n);
	wf_path_leaf(L"\\\\srv\\share\\d", &l, &n);
	CHECK(1 == n && L'd' == l[0]);
}

static void test_join(void)
{
	wchar_t *r = NULL;

	CHECK(WF_OK == wf_path_join(L"C:\\a", L"b", &r) &&
	        0 == wcscmp(r, L"C:\\a\\b"));
	free(r);
	CHECK(WF_OK == wf_path_join(L"C:\\", L"b", &r) &&
	        0 == wcscmp(r, L"C:\\b"));
	free(r);
	r = NULL;
	CHECK(WF_E_INVAL == wf_path_join(L"C:\\a", L"b\\c", &r));
	CHECK(WF_E_INVAL == wf_path_join(L"C:\\a", L"b/c", &r));
	CHECK(WF_E_INVAL == wf_path_join(L"C:\\a", L"", &r));
	CHECK(NULL == r);
}

static void test_extended(void)
{
	CHECK(ext_is(L"C:\\a\\b", L"\\\\?\\C:\\a\\b"));
	CHECK(ext_is(L"C:\\", L"\\\\?\\C:\\"));
	CHECK(ext_is(L"\\\\srv\\sh\\x", L"\\\\?\\UNC\\srv\\sh\\x"));
	CHECK(ext_is(L"\\\\?\\C:\\a", L"\\\\?\\C:\\a"));
	/* \\?\ disables normalization, so these must never get through */
	CHECK(ext_rejected(L"C:\\a\\..\\b"));
	CHECK(ext_rejected(L"C:\\a\\.\\b"));
	CHECK(ext_rejected(L"C:/a/b"));
	CHECK(ext_rejected(L"rel\\path"));
	CHECK(ext_rejected(L"C:"));
	CHECK(ext_rejected(L""));
	/* names that merely contain dots are fine */
	CHECK(ext_is(L"C:\\a\\..b\\c.", L"\\\\?\\C:\\a\\..b\\c."));
}

/* wf_path_dup copies the whole string, and nothing for NULL. */
static void test_dup(void)
{
	const wchar_t *src = L"C:\\a b\\c";
	wchar_t *d = wf_path_dup(src);
	wchar_t *e = wf_path_dup(L"");

	CHECK(d && d != src && !wcscmp(d, src));
	CHECK(e && 0 == e[0]);
	CHECK(NULL == wf_path_dup(NULL));
	{
		wchar_t *empty = wf_path_dup_n(NULL, 0);

		CHECK(empty && !empty[0]);
		CHECK(NULL == wf_path_dup_n(NULL, 1));
		free(empty);
	}
	free(d);
	free(e);
}

int main(void)
{
	test_parent();
	test_leaf();
	test_join();
	test_extended();
	test_dup();
	return TEST_RESULT();
}
