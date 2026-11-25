/* Partial process lists and unresolved ancestry must never authorize an action. */
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <wchar.h>

#include "base/wf_macro.h"
#include "wf_test.h"

typedef enum
{
	SNAP_COMPLETE,
	SNAP_FIRST_FAILED,
	SNAP_NEXT_FAILED
} SnapshotMode;
static SnapshotMode snapshot_mode;
static size_t snapshot_index;

static HANDLE WINAPI snapshot_create(DWORD flags, DWORD pid)
{
	(void)flags;
	(void)pid;
	snapshot_index = 0;
	return (HANDLE)(uintptr_t)1;
}

static BOOL WINAPI snapshot_first(HANDLE handle, LPPROCESSENTRY32W entry)
{
	(void)handle;
	if (SNAP_FIRST_FAILED == snapshot_mode) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	entry->th32ProcessID = 10;
	entry->th32ParentProcessID = 20;
	return TRUE;
}

static BOOL WINAPI snapshot_next(HANDLE handle, LPPROCESSENTRY32W entry)
{
	(void)handle;
	if (SNAP_NEXT_FAILED == snapshot_mode) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	if (snapshot_index++) {
		SetLastError(ERROR_NO_MORE_FILES);
		return FALSE;
	}
	entry->th32ProcessID = 20;
	entry->th32ParentProcessID = 0;
	return TRUE;
}

static BOOL WINAPI snapshot_close(HANDLE handle)
{
	return handle == (HANDLE)(uintptr_t)1 ? TRUE : CloseHandle(handle);
}

static UINT WINAPI windows_directory(LPWSTR out, UINT cap)
{
	static const wchar_t dir[] = L"C:\\Windows";

	if (cap < WF_ARRAY_LEN(dir)) {
		return (UINT)WF_ARRAY_LEN(dir);
	}
	wmemcpy(out, dir, WF_ARRAY_LEN(dir));
	return (UINT)WF_ARRAY_LEN(dir) - 1;
}

#define CreateToolhelp32Snapshot snapshot_create
#define Process32FirstW snapshot_first
#define Process32NextW snapshot_next
#define CloseHandle snapshot_close
#define GetSystemWindowsDirectoryW windows_directory
#ifndef WF_PROCESS_CONTROL_SOURCE
#	define WF_PROCESS_CONTROL_SOURCE                                      \
		"../src/process/wf_process_control_win.c"
#endif
#include WF_PROCESS_CONTROL_SOURCE
#undef GetSystemWindowsDirectoryW
#undef CloseHandle
#undef Process32NextW
#undef Process32FirstW
#undef CreateToolhelp32Snapshot

static void incomplete_lists_are_unknown(void)
{
	const SnapshotMode modes[] = {
	        SNAP_COMPLETE, SNAP_FIRST_FAILED, SNAP_NEXT_FAILED};

	for (size_t i = 0; i < WF_ARRAY_LEN(modes); ++i) {
		WfProcSnap snap = {0};
		bool known = false;

		snapshot_mode = modes[i];
		CHECK(!is_ancestor(&snap, 10, 99, &known));
		CHECK(known == (modes[i] == SNAP_COMPLETE));
		CHECK(snap.ok == (modes[i] == SNAP_COMPLETE));
		CHECK(snap.reads == 1);
		wf_proc_snap_free(&snap);
	}
}

static void unresolved_chains_are_unknown(void)
{
	DWORD ids[] = {10, 20, 30};
	DWORD parents[] = {20, 30, 0};
	WfProcSnap snap = {.ids = ids,
	        .parents = parents,
	        .n = WF_ARRAY_LEN(ids),
	        .read = true,
	        .ok = true};
	bool known = false;

	CHECK(is_ancestor(&snap, 10, 30, &known) && known);
	CHECK(!is_ancestor(&snap, 10, 99, &known) && known);
	CHECK(!is_ancestor(&snap, 99, 30, &known) && !known);
	snap.n = 1;
	CHECK(!is_ancestor(&snap, 10, 99, &known) && !known);
	snap.n = WF_ARRAY_LEN(ids);
	parents[2] = 10;
	CHECK(!is_ancestor(&snap, 10, 99, &known) && !known);

	DWORD long_ids[65], long_parents[65];
	for (size_t i = 0; i < WF_ARRAY_LEN(long_ids); ++i) {
		long_ids[i] = (DWORD)i + 1;
		long_parents[i] =
		        i + 1 < WF_ARRAY_LEN(long_ids) ? (DWORD)i + 2 : 0;
	}
	snap.ids = long_ids;
	snap.parents = long_parents;
	snap.n = WF_ARRAY_LEN(long_ids);
	CHECK(!is_ancestor(&snap, 1, 99, &known) && !known);
}

static void windows_path_requires_a_component_boundary(void)
{
	CHECK(under_windows_dir(L"C:\\Windows\\System32\\conhost.exe"));
	CHECK(under_windows_dir(L"c:\\windows\\explorer.exe"));
	CHECK(!under_windows_dir(L"C:\\WindowsBackup\\conhost.exe"));
	CHECK(!under_windows_dir(L"C:\\tools\\conhost.exe"));
}

int main(void)
{
	incomplete_lists_are_unknown();
	unresolved_chains_are_unknown();
	windows_path_requires_a_component_boundary();
	return TEST_RESULT();
}
