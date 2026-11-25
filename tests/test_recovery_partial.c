/* Real store and deletion code; only failing Win32 calls are injected. */
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "fs/owned_delete.h"
#include "recovery/wf_recovery_store.h"
#include "wf_test.h"
static int fail_meta_create, fail_meta_delete, fail_intent_flush,
        fail_completion;
static int fail_query, fail_alloc, short_read, short_write;
static HANDLE audit_handle;
static int suffix(const wchar_t *path, const wchar_t *tail)
{
	size_t n = wcslen(path), m = wcslen(tail);
	return n >= m && !wcscmp(path + n - m, tail);
}
HANDLE WINAPI fault_open(LPCWSTR path,
        DWORD access,
        DWORD share,
        LPSECURITY_ATTRIBUTES sa,
        DWORD creation,
        DWORD flags,
        HANDLE template_file)
{
	HANDLE h;
	if (fail_meta_create && CREATE_NEW == creation &&
	        suffix(path, L".meta")) {
		SetLastError(ERROR_DISK_FULL);
		return INVALID_HANDLE_VALUE;
	}
	h = CreateFileW(
	        path, access, share, sa, creation, flags, template_file);
	if (INVALID_HANDLE_VALUE != h && suffix(path, L"recovery.log")) {
		audit_handle = h;
	}
	return h;
}
BOOL WINAPI fault_write(HANDLE h,
        LPCVOID bytes,
        DWORD count,
        LPDWORD wrote,
        LPOVERLAPPED overlap)
{
	if (short_write) {
		*wrote = count ? count - 1 : 0;
		return TRUE;
	}
	if (fail_completion && h == audit_handle && count >= 19 &&
	        !memcmp(bytes, "RECOVERY_ABANDONED ", 19)) {
		*wrote = 0;
		SetLastError(ERROR_DISK_FULL);
		return FALSE;
	}
	return WriteFile(h, bytes, count, wrote, overlap);
}
BOOL WINAPI fault_read(
        HANDLE h, LPVOID bytes, DWORD count, LPDWORD read, LPOVERLAPPED overlap)
{
	if (short_read) {
		*read = count ? count - 1 : 0;
		return TRUE;
	}
	return ReadFile(h, bytes, count, read, overlap);
}
BOOL WINAPI fault_query(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID bytes, DWORD count)
{
	if (fail_query) {
		SetLastError(ERROR_NOT_READY);
		return FALSE;
	}
	return GetFileInformationByHandleEx(h, kind, bytes, count);
}
static void *fault_realloc(void *memory, size_t bytes)
{
	return fail_alloc ? NULL : realloc(memory, bytes);
}
BOOL WINAPI fault_flush(HANDLE h)
{
	if (fail_intent_flush && h == audit_handle) {
		SetLastError(ERROR_WRITE_FAULT);
		return FALSE;
	}
	return FlushFileBuffers(h);
}
BOOL WINAPI fault_disposition(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID bytes, DWORD count)
{
	wchar_t path[2048];
	DWORD n;
	if (fail_meta_delete &&
	        (kind == FileDispositionInfoEx ||
	                kind == FileDispositionInfo)) {
		n = GetFinalPathNameByHandleW(h, path, 2048, 0);
		if (n && n < 2048 && suffix(path, L".meta")) {
			SetLastError(ERROR_ACCESS_DENIED);
			return FALSE;
		}
	}
	return SetFileInformationByHandle(h, kind, bytes, count);
}
#define CreateFileW fault_open
#define WriteFile fault_write
#define ReadFile fault_read
#define GetFileInformationByHandleEx fault_query
#define FlushFileBuffers fault_flush
#define SetFileInformationByHandle fault_disposition
#define id_equal owned_test_id_equal
#ifndef WF_RECOVERY_OWNED_SOURCE
#	define WF_RECOVERY_OWNED_SOURCE "../src/fs/owned_delete.c"
#endif
#include WF_RECOVERY_OWNED_SOURCE
#undef id_equal
#define realloc fault_realloc
#ifndef WF_RECOVERY_STORE_SOURCE
#	define WF_RECOVERY_STORE_SOURCE "../src/recovery/wf_recovery_store.c"
#endif
#include WF_RECOVERY_STORE_SOURCE
#undef realloc
#undef CreateFileW
#undef WriteFile
#undef ReadFile
#undef GetFileInformationByHandleEx
#undef FlushFileBuffers
#undef SetFileInformationByHandle
static const wchar_t recovery_id[] = L"12345678-1234-1234-1234-123456789abc";
typedef struct
{
	P0Lab lab;
	wchar_t root[2048];
	WfRecoveryStore *store;
} PartialFixture;
static int fixture_open(PartialFixture *f)
{
	DWORD error;
	memset(f, 0, sizeof *f);
	audit_handle = NULL;
	return p0_lab_open(&f->lab, "recovery-partial", 0) &&
	        p0_lab_path(&f->lab, L"state", f->root, 2048) &&
	        wf_recovery_store_open(f->root, 1, &f->store, &error);
}
static void restart(PartialFixture *f)
{
	DWORD error;
	wf_recovery_store_close(f->store);
	f->store = NULL;
	audit_handle = NULL;
	CHECK(wf_recovery_store_open(f->root, 0, &f->store, &error));
}
static void keep(PartialFixture *f)
{
	wf_recovery_store_close(f->store);
	CHECK(0 == p0_lab_close(&f->lab, 1));
}
static void remove_fixture_object(PartialFixture *f, const wchar_t *extension)
{
	wchar_t path[2048];
	CHECK(swprintf(path, 2048, L"%ls\\%ls%ls", f->root, recovery_id,
	              extension) > 0);
	CHECK(DeleteFileW(path));
}
static void check_held(PartialFixture *f, WfRecoveryLocalState state)
{
	WfRecoveryLocalInfo info;
	WfRecoveryAbandonReport report;
	DWORD error;
	restart(f);
	CHECK(1 ==
	        wf_recovery_store_inspect(
	                f->store, recovery_id, &info, &error));
	CHECK(info.state == state && WF_RECOVERY_ABANDON_HOLD == info.action);
	CHECK(!wf_recovery_store_abandon(
	        f->store, recovery_id, recovery_id, &report));
	CHECK(ERROR_INVALID_DATA == report.error && !report.journal_deleted &&
	        !report.metadata_deleted);
	CHECK(report.state_after == state);
}
static void additional_partial_states(void)
{
	PartialFixture f;
	DWORD error;
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	remove_fixture_object(&f, L".wjr");
	check_held(&f, WF_RECOVERY_LOCAL_METADATA_ONLY);
	keep(&f);
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	CHECK(log_event(
	        f.store, recovery_id, "RECOVERY_ABANDON_INTENT", &error));
	remove_fixture_object(&f, L".meta");
	check_held(&f, WF_RECOVERY_LOCAL_ABANDON_METADATA_DELETED);
	keep(&f);
}
static void failures_and_resources(void)
{
	PartialFixture f;
	WfRecoveryLocalInfo info;
	WfRecoveryAbandonReport report;
	DWORD error, before, after;
	unsigned i;
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (i = 0; i < 32; i++) {
		fail_query = 1;
		CHECK(wf_recovery_store_refresh(f.store, &error));
		CHECK(wf_recovery_store_inspect(
		              f.store, recovery_id, &info, &error) == 1);
		CHECK(WF_RECOVERY_LOCAL_MANUAL == info.state &&
		        ERROR_NOT_READY == info.error);
		CHECK(!wf_recovery_store_abandon(
		        f.store, recovery_id, recovery_id, &report));
		CHECK(ERROR_NOT_READY == report.error &&
		        !report.journal_deleted && !report.metadata_deleted);
		fail_query = 0;
		short_read = 1;
		CHECK(wf_recovery_store_refresh(f.store, &error));
		CHECK(wf_recovery_store_inspect(
		              f.store, recovery_id, &info, &error) == 1);
		CHECK(WF_RECOVERY_LOCAL_MANUAL == info.state &&
		        ERROR_HANDLE_EOF == info.error);
		short_read = 0;
		fail_alloc = 1;
		CHECK(!wf_recovery_store_refresh(f.store, &error));
		CHECK(ERROR_NOT_ENOUGH_MEMORY == error);
		fail_alloc = 0;
		CHECK(wf_recovery_store_refresh(f.store, &error));
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	CHECK(before == after);
	CHECK(1 ==
	                wf_recovery_store_inspect(
	                        f.store, recovery_id, &info, &error) &&
	        WF_RECOVERY_LOCAL_READY == info.state);
	CHECK(wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	keep(&f);
	CHECK(fixture_open(&f));
	short_write = 1;
	CHECK(!wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	CHECK(ERROR_WRITE_FAULT == error);
	short_write = 0;
	restart(&f);
	CHECK(1 ==
	        wf_recovery_store_inspect(f.store, recovery_id, &info, &error));
	CHECK(WF_RECOVERY_LOCAL_MANUAL == info.state &&
	        ERROR_INVALID_DATA == info.error);
	keep(&f);
}
static void unknown_audit_and_objects(void)
{
	PartialFixture f;
	WfRecoveryLocalInfo info;
	WfRecoveryAbandonReport report;
	DWORD error, wrote;
	wchar_t path[2048];
	HANDLE h;
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	CHECK(swprintf(path, 2048, L"%ls\\USER", f.root) > 0);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CloseHandle(h);
	restart(&f);
	memset(&info, 0xa5, sizeof info);
	CHECK(-1 == wf_recovery_store_get(f.store, 0, &info, &error) &&
	        ERROR_INVALID_DATA == error);
	CHECK(WF_RECOVERY_LOCAL_MANUAL == info.state &&
	        ERROR_INVALID_DATA == info.error && info.reason);
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(!report.journal_deleted && !report.metadata_deleted &&
	        ERROR_INVALID_DATA == report.error);
	CHECK(INVALID_FILE_ATTRIBUTES != GetFileAttributesW(path));
	keep(&f);
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	CHECK(swprintf(path, 2048, L"%ls\\recovery.log", f.root) > 0);
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
	        FILE_ATTRIBUTE_NORMAL, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, "RECOVERY_ABANDON_INTENT torn", 27, &wrote, NULL) &&
	        27 == wrote);
	CloseHandle(h);
	restart(&f);
	CHECK(-1 ==
	        wf_recovery_store_inspect(f.store, recovery_id, &info, &error));
	CHECK(WF_RECOVERY_LOCAL_MANUAL == info.state &&
	        ERROR_INVALID_DATA == error);
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(!report.journal_deleted && !report.metadata_deleted);
	keep(&f);
}
int main(void)
{
	PartialFixture f;
	WfRecoveryLocalInfo info;
	WfRecoveryAbandonReport report;
	DWORD error;
	CHECK(fixture_open(&f));
	fail_meta_create = 1;
	CHECK(!wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	CHECK(ERROR_DISK_FULL == error);
	fail_meta_create = 0;
	restart(&f);
	CHECK(1 ==
	        wf_recovery_store_inspect(f.store, recovery_id, &info, &error));
	CHECK(WF_RECOVERY_LOCAL_JOURNAL_ONLY == info.state &&
	        WF_RECOVERY_ABANDON_HOLD == info.action);
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(!report.journal_deleted && !report.metadata_deleted);
	keep(&f);
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	fail_intent_flush = 1;
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(ERROR_WRITE_FAULT == report.error && !report.journal_deleted &&
	        !report.metadata_deleted);
	fail_intent_flush = 0;
	restart(&f);
	CHECK(1 ==
	        wf_recovery_store_inspect(f.store, recovery_id, &info, &error));
	CHECK(WF_RECOVERY_LOCAL_ABANDON_INTENT == info.state &&
	        WF_RECOVERY_ABANDON_PAIR == info.action);
	CHECK(wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	keep(&f);
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	fail_meta_delete = 1;
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(ERROR_ACCESS_DENIED == report.error &&
	        1 == report.journal_deleted && !report.metadata_deleted);
	fail_meta_delete = 0;
	restart(&f);
	CHECK(1 ==
	        wf_recovery_store_inspect(f.store, recovery_id, &info, &error));
	CHECK(WF_RECOVERY_LOCAL_ABANDON_JOURNAL_DELETED == info.state &&
	        WF_RECOVERY_ABANDON_HOLD == info.action);
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	keep(&f);
	CHECK(fixture_open(&f));
	CHECK(wf_recovery_store_put(
	        f.store, recovery_id, L"REMOVED-VOLUME", "bytes", 5, &error));
	fail_completion = 1;
	CHECK(!wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(ERROR_DISK_FULL == report.error && 1 == report.journal_deleted &&
	        1 == report.metadata_deleted && !report.logged);
	fail_completion = 0;
	restart(&f);
	CHECK(1 ==
	        wf_recovery_store_inspect(f.store, recovery_id, &info, &error));
	CHECK(WF_RECOVERY_LOCAL_ABANDON_AUDIT_PENDING == info.state &&
	        WF_RECOVERY_ABANDON_AUDIT_ONLY == info.action);
	CHECK(wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(report.logged && !report.journal_deleted &&
	        !report.metadata_deleted);
	CHECK(1 ==
	                wf_recovery_store_inspect(
	                        f.store, recovery_id, &info, &error) &&
	        WF_RECOVERY_LOCAL_ABANDONED == info.state);
	CHECK(wf_recovery_store_abandon(
	        f.store, recovery_id, recovery_id, &report));
	CHECK(WF_RECOVERY_ABANDON_DONE == report.action &&
	        !report.journal_deleted && !report.metadata_deleted);
	keep(&f);
	additional_partial_states();
	failures_and_resources();
	unknown_audit_and_objects();
	return TEST_RESULT();
}
