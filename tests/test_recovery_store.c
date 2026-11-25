#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "recovery/wf_recovery_store.h"
#include "wf_test.h"
/* Trace the real module's path-bearing Win32 boundary. An accidental attempt
 * to open a recorded unavailable volume fails and increments target_io. */
static unsigned target_io;
static HANDLE audit_handle;
static int block_audit;
static int target_path(const wchar_t *p)
{
	if (p &&
	        (wcsstr(p, L"unavailable.invalid") ||
	                wcsstr(p, L"ffffffff-ffff-ffff-ffff-ffffffffffff"))) {
		target_io++;
		SetLastError(ERROR_ACCESS_DENIED);
		return 1;
	}
	return 0;
}
static HANDLE WINAPI tracked_open(LPCWSTR p,
        DWORD a,
        DWORD s,
        LPSECURITY_ATTRIBUTES sa,
        DWORD c,
        DWORD f,
        HANDLE t)
{
	HANDLE h = target_path(p) ? INVALID_HANDLE_VALUE
	                          : CreateFileW(p, a, s, sa, c, f, t);
	if (INVALID_HANDLE_VALUE != h && wcsstr(p, L"\\recovery.log")) {
		audit_handle = h;
	}
	return h;
}
static BOOL WINAPI tracked_write(HANDLE h,
        LPCVOID bytes,
        DWORD count,
        LPDWORD wrote,
        LPOVERLAPPED overlap)
{
	if (h == audit_handle && block_audit) {
		SetLastError(ERROR_DISK_FULL);
		*wrote = 0;
		return FALSE;
	}
	return WriteFile(h, bytes, count, wrote, overlap);
}
static BOOL WINAPI tracked_mkdir(LPCWSTR p, LPSECURITY_ATTRIBUTES a)
{
	return target_path(p) ? FALSE : CreateDirectoryW(p, a);
}
static DWORD WINAPI tracked_attributes(LPCWSTR p)
{
	return target_path(p) ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(p);
}
static HANDLE WINAPI tracked_find(LPCWSTR p, LPWIN32_FIND_DATAW data)
{
	return target_path(p) ? INVALID_HANDLE_VALUE : FindFirstFileW(p, data);
}
static UINT WINAPI tracked_drive(LPCWSTR p)
{
	return target_path(p) ? DRIVE_UNKNOWN : GetDriveTypeW(p);
}
#define WriteFile tracked_write
#define CreateFileW tracked_open
#define CreateDirectoryW tracked_mkdir
#define GetFileAttributesW tracked_attributes
#define FindFirstFileW tracked_find
#define GetDriveTypeW tracked_drive
#ifndef WF_RECOVERY_STORE_SOURCE
#	define WF_RECOVERY_STORE_SOURCE "../src/recovery/wf_recovery_store.c"
#endif
#include WF_RECOVERY_STORE_SOURCE
#undef WriteFile
#undef CreateFileW
#undef CreateDirectoryW
#undef GetFileAttributesW
#undef FindFirstFileW
#undef GetDriveTypeW
int main(void)
{
	static const wchar_t id[] = L"01000000-0000-0000-0000-000000000001";
	static const wchar_t second[] = L"01000000-0000-0000-0000-000000000002";
	P0Lab lab;
	wchar_t root[2048], target[2048];
	DWORD error;
	WfRecoveryStore *store = NULL;
	WfRecoveryLocalInfo info;
	WfRecoveryAbandonReport report;
	CHECK(p0_lab_open(&lab, "abandon", 0));
	CHECK(p0_lab_path(&lab, L"local-state", root, 2048));
	CHECK(p0_lab_path(&lab, L"target-data", target, 2048));
	CHECK(CreateDirectoryW(target, NULL));
	/* The volume strings are deliberately unreachable. No API may interpret
	 * them as a path or attempt target-volume I/O during listing/abandon.
	 */
	CHECK(wf_recovery_store_open(root, 1, &store, &error));
	if (!store) {
		p0_lab_close(&lab, 1);
		return TEST_RESULT();
	}
	CHECK(wf_recovery_store_put(store, id,
	        L"\\\\unavailable.invalid\\share", "journal", 7, &error));
	CHECK(wf_recovery_store_put(store, second,
	        L"\\\\?\\Volume{ffffffff-ffff-ffff-ffff-ffffffffffff}\\",
	        "second", 6, &error));
	CHECK(1 == wf_recovery_store_get(store, 0, &info, &error));
	CHECK(1 == wf_recovery_store_get(store, 1, &info, &error));
	CHECK(!wf_recovery_store_abandon(store, id, second, &report));
	CHECK(!report.journal_deleted && !report.metadata_deleted);
	CHECK(1 == wf_recovery_store_get(store, 1, &info, &error));
	block_audit = 1;
	CHECK(!wf_recovery_store_abandon(store, id, id, &report));
	CHECK(!report.journal_deleted && !report.metadata_deleted);
	CHECK(1 == wf_recovery_store_get(store, 1, &info, &error));
	block_audit = 0;
	CHECK(wf_recovery_store_abandon(store, id, id, &report));
	CHECK(1 == report.journal_deleted && 1 == report.metadata_deleted &&
	        report.intent_logged && report.logged);
	CHECK(1 == wf_recovery_store_get(store, 0, &info, &error));
	CHECK(!wcscmp(info.recovery_id, second));
	CHECK(0 == wf_recovery_store_get(store, 1, &info, &error));
	CHECK(GetFileAttributesW(target) & FILE_ATTRIBUTE_DIRECTORY);
	wf_recovery_store_close(store);
	store = NULL;
	CHECK(wf_recovery_store_open(root, 0, &store, &error));
	CHECK(1 == wf_recovery_store_get(store, 0, &info, &error));
	CHECK(!wcscmp(info.recovery_id, second));
	wf_recovery_store_close(store);
	CHECK(0 == target_io);
	printf("target_volume_io=%u; exact_id_confirmation=verified; "
	       "local_metadata_only=verified; "
	       "durable_audit_before_delete=verified\n",
	        target_io);
	CHECK(0 == p0_lab_close(&lab, 1));
	return TEST_RESULT();
}
