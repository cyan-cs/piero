#include <windows.h>
#include <stdbool.h>
#include <wchar.h>
#include "wf_test.h"

static unsigned creates, writes, closes, deletes, fail_create, existing;
static bool fail_write, fail_cleanup;
static LSTATUS WINAPI create_key(HKEY root,
        LPCWSTR path,
        DWORD reserved,
        LPWSTR cls,
        DWORD options,
        REGSAM access,
        const SECURITY_ATTRIBUTES *sa,
        PHKEY result,
        LPDWORD disposition)
{
	(void)root;
	(void)path;
	(void)reserved;
	(void)cls;
	(void)options;
	(void)access;
	(void)sa;
	creates++;
	if (creates == fail_create) {
		return ERROR_ACCESS_DENIED;
	}
	*result = (HKEY)(uintptr_t)creates;
	*disposition = creates == existing ? REG_OPENED_EXISTING_KEY
	                                   : REG_CREATED_NEW_KEY;
	return ERROR_SUCCESS;
}
static LSTATUS WINAPI set_value(HKEY key,
        LPCWSTR name,
        DWORD reserved,
        DWORD type,
        const BYTE *data,
        DWORD size)
{
	(void)key;
	(void)name;
	(void)reserved;
	(void)type;
	(void)data;
	(void)size;
	writes++;
	return fail_write ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
static LSTATUS WINAPI close_key(HKEY key)
{
	CHECK(NULL != key);
	closes++;
	return ERROR_SUCCESS;
}
static LSTATUS WINAPI delete_key(HKEY root, LPCWSTR path)
{
	(void)root;
	(void)path;
	deletes++;
	return fail_cleanup ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
#define RegCreateKeyExW create_key
#define RegSetValueExW set_value
#define RegCloseKey close_key
#define RegDeleteTreeW delete_key
#include "wf_test_assoc.h"

static WfTestAssoc fresh(void)
{
	WfTestAssoc a;
	CHECK(ERROR_SUCCESS == wf_test_assoc_name(&a));
	creates = writes = closes = deletes = fail_create = existing = 0;
	fail_write = fail_cleanup = false;
	return a;
}
int main(void)
{
	WfTestAssoc a = fresh(), b = fresh();
	CHECK(wcscmp(a.ext, b.ext) && wcscmp(a.progid, b.progid));
	fail_create = 1;
	CHECK(ERROR_ACCESS_DENIED ==
	        wf_test_assoc_register(&a, L"C:\\owned\\target.exe"));
	CHECK(!writes && !closes && !deletes && !a.ext_owned &&
	        !a.progid_owned);
	a = fresh();
	existing = 1;
	CHECK(ERROR_ALREADY_EXISTS ==
	        wf_test_assoc_register(&a, L"C:\\owned\\target.exe"));
	CHECK(!writes && !deletes && 1 == closes);
	a = fresh();
	existing = 2;
	CHECK(ERROR_ALREADY_EXISTS ==
	        wf_test_assoc_register(&a, L"C:\\owned\\target.exe"));
	CHECK(1 == writes && 1 == deletes && 2 == closes && !a.ext_owned &&
	        !a.progid_owned);
	a = fresh();
	fail_create = 3;
	CHECK(ERROR_ACCESS_DENIED ==
	        wf_test_assoc_register(&a, L"C:\\owned\\target.exe"));
	CHECK(2 == deletes && 2 == closes && !a.ext_owned && !a.progid_owned);
	a = fresh();
	fail_write = fail_cleanup = true;
	CHECK(ERROR_ACCESS_DENIED ==
	        wf_test_assoc_register(&a, L"C:\\owned\\target.exe"));
	CHECK(a.ext_owned && ERROR_ACCESS_DENIED == a.cleanup_error);
	fail_cleanup = false;
	CHECK(ERROR_SUCCESS == wf_test_assoc_cleanup(&a) && !a.ext_owned);
	a = fresh();
	CHECK(ERROR_SUCCESS ==
	        wf_test_assoc_register(&a, L"C:\\owned\\target.exe"));
	CHECK(2 == writes && 3 == closes && a.ext_owned && a.progid_owned);
	CHECK(ERROR_SUCCESS == wf_test_assoc_cleanup(&a) && 2 == deletes);
	CHECK(ERROR_SUCCESS == wf_test_assoc_cleanup(&a) && 2 == deletes);
	return TEST_RESULT();
}
