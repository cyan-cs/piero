#include <stdio.h>
#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "../tools/p0/p0_source_guard.h"
#include "wf_test.h"
#include "xfer/wf_probe_win.h"

static FILE_INFO_BY_HANDLE_CLASS fail_class;
static BOOL failed_query(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, void *data, DWORD count)
{
	if (cls == fail_class) {
		SetLastError(ERROR_CRC);
		return FALSE;
	}
	return GetFileInformationByHandleEx(h, cls, data, count);
}
#define GetFileInformationByHandleEx failed_query
#include "../tools/p0/p0_source_guard.c"
#undef GetFileInformationByHandleEx

int main(void)
{
	P0Lab lab;
	wchar_t path[1024];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo snapshot;
	FILE_INFO_BY_HANDLE_CLASS classes[] = {
	        FileIdInfo, FileStandardInfo, FileBasicInfo};
	HANDLE h;
	DWORD written, before, after, error;
	unsigned i;
	CHECK(p0_lab_open(&lab, "guard-fault", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"source", path, 1024));
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, "fixture", 7, &written, NULL));
	CloseHandle(h);
	CHECK(p0_area_track(lab.area, L"source", 0));
	CHECK(WF_OK == ops.node(NULL, path, &snapshot));
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (i = 0; i < 3; i++) {
		P0SourceGuard guard;
		P0GuardResult result;
		fail_class = classes[i];
		result = p0_source_guard_acquire(
		        path, &snapshot, &guard, &error);
		CHECK(P0_SOURCE_BUSY == result);
		CHECK(ERROR_CRC == error &&
		        INVALID_HANDLE_VALUE == guard.handle);
		printf("%s injected_query_class=%u expected=SOURCE_BUSY "
		       "actual=%d expected_error=%lu actual_error=%lu "
		       "source_volume=%llu\n",
		        P0_SOURCE_BUSY == result && ERROR_CRC == error &&
		                        INVALID_HANDLE_VALUE == guard.handle
		                ? "PASS"
		                : "FAIL",
		        (unsigned)classes[i], result, (DWORD)ERROR_CRC, error,
		        (unsigned long long)snapshot.id.volume);
		p0_source_guard_release(&guard);
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
	CHECK(before == after);
	printf("query failures: ID/size/type/time; error=%lu; handles "
	       "before=%lu after=%lu\n",
	        error, before, after);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
