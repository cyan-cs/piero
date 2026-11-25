#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "../tools/p0/p0_source_guard.h"
#include "wf_test.h"
#include "xfer/wf_probe_win.h"

int main(void)
{
	P0Lab lab;
	P0SourceGuard guard;
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo snapshot;
	wchar_t source[1024], renamed[1024];
	HANDLE file;
	DWORD written, error, before, after;
	P0GuardResult result;
	CHECK(p0_lab_open(&lab, "guard-unit", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"source.bin", source, 1024));
	CHECK(p0_lab_path(&lab, L"adopted.bin", renamed, 1024));
	file = CreateFileW(source, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != file);
	CHECK(WriteFile(file, "fixture", 7, &written, NULL) && 7 == written);
	CloseHandle(file);
	CHECK(p0_area_track(lab.area, L"source.bin", 0));
	CHECK(WF_OK == ops.node(NULL, source, &snapshot));
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	result = p0_source_guard_acquire(source, &snapshot, &guard, &error);
	CHECK(P0_GUARD_OK == result && 0 == error &&
	        INVALID_HANDLE_VALUE != guard.handle);
	if (P0_GUARD_OK == result) {
		file = CreateFileW(source, GENERIC_WRITE,
		        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		        NULL, OPEN_EXISTING, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE == file &&
		        ERROR_SHARING_VIOLATION == GetLastError());
		if (INVALID_HANDLE_VALUE != file) {
			CloseHandle(file);
		}
		CHECK(!MoveFileExW(source, renamed, 0) &&
		        ERROR_SHARING_VIOLATION == GetLastError());
		p0_source_guard_release(&guard);
		p0_source_guard_release(&guard);
		CHECK(INVALID_HANDLE_VALUE == guard.handle);
	}
	snapshot.id.id[0] ^= 0xff;
	CHECK(P0_SOURCE_CHANGED ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	CHECK(INVALID_HANDLE_VALUE == guard.handle);
	p0_source_guard_release(&guard);
	snapshot.id.id[0] ^= 0xff;
	snapshot.id.volume++;
	CHECK(P0_SOURCE_CHANGED ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	p0_source_guard_release(&guard);
	snapshot.id.volume--;
	snapshot.size++;
	CHECK(P0_SOURCE_CHANGED ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	p0_source_guard_release(&guard);
	snapshot.size--;
	snapshot.mtime++;
	CHECK(P0_SOURCE_CHANGED ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	p0_source_guard_release(&guard);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	CHECK(before == after);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
