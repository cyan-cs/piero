#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "../tools/p0/p0_source_guard.h"
#include "path/wf_canonical_win.h"
#include "wf_test.h"
#include "xfer/wf_copy_source_win.h"
#include "xfer/wf_probe_win.h"
static FILE_INFO_BY_HANDLE_CLASS fail_class;
static int fail_query, fail_path, alter_path, fail_duplicate;
static BOOL query(
        HANDLE h, FILE_INFO_BY_HANDLE_CLASS cls, void *bytes, DWORD size)
{
	if (fail_query && cls == fail_class) {
		SetLastError(ERROR_CRC);
		return FALSE;
	}
	return GetFileInformationByHandleEx(h, cls, bytes, size);
}
static int fault_canonical(HANDLE h, WfCanonicalPath *out, DWORD *error)
{
	if (fail_path) {
		*error = ERROR_INVALID_NAME;
		return 0;
	}
	if (!wf_canonical_from_handle(h, out, error)) {
		return 0;
	}
	if (alter_path) {
		out->path[wcslen(out->path) - 1] = L'X';
	}
	return 1;
}
static BOOL WINAPI duplicate(HANDLE a,
        HANDLE b,
        HANDLE c,
        LPHANDLE out,
        DWORD access,
        BOOL inherit,
        DWORD flags)
{
	if (fail_duplicate) {
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}
	return DuplicateHandle(a, b, c, out, access, inherit, flags);
}
#define GetFileInformationByHandleEx query
#define wf_canonical_from_handle fault_canonical
#define DuplicateHandle duplicate
#include "../src/xfer/wf_copy_source_win.c"
#include "base/wf_macro.h"
#undef GetFileInformationByHandleEx
#undef wf_canonical_from_handle
#undef DuplicateHandle
int main(void)
{
	P0Lab lab;
	WfProbeOps ops = wf_probe_win();
	P0SourceGuard guard;
	WfNodeInfo snapshot;
	WfCopySourceObserver *o = NULL;
	WfCopySourceReport report;
	FILE_INFO_BY_HANDLE_CLASS classes[] = {
	        FileIdInfo, FileStandardInfo, FileBasicInfo};
	wchar_t source[1024];
	HANDLE h;
	DWORD error, written, before, after;
	CHECK(p0_lab_open(&lab, "copy-source-fault", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"source", source, 1024));
	h = CreateFileW(source, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h &&
	        WriteFile(h, "body", 4, &written, NULL) && 4 == written);
	CloseHandle(h);
	CHECK(p0_area_track(lab.area, L"source", 0));
	CHECK(WF_OK == ops.node(NULL, source, &snapshot));
	CHECK(P0_GUARD_OK ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	CHECK(wf_copy_source_begin(guard.handle, &snapshot, &o, &report));
	wf_copy_source_release(o);
	o = NULL;
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (unsigned i = 0; i < WF_ARRAY_LEN(classes); i++) {
		fail_query = 1;
		fail_class = classes[i];
		CHECK(!wf_copy_source_begin(
		        guard.handle, &snapshot, &o, &report));
		CHECK(NULL == o && ERROR_CRC == report.error);
		fail_query = 0;
		CHECK(wf_copy_source_begin(
		        guard.handle, &snapshot, &o, &report));
		fail_query = 1;
		CHECK(!wf_copy_source_observe(o, guard.handle, 1, &report));
		CHECK(ERROR_CRC == report.error);
		fail_query = 0;
		CHECK(!wf_copy_source_finish(o, 1, 0, &report));
		printf("query_class=%u actual_error=%lu accepted=0\n",
		        (unsigned)classes[i], report.error);
		wf_copy_source_release(o);
		o = NULL;
	}
	fail_path = 1;
	CHECK(!wf_copy_source_begin(guard.handle, &snapshot, &o, &report));
	CHECK(NULL == o && ERROR_INVALID_NAME == report.error);
	fail_path = 0;
	CHECK(wf_copy_source_begin(guard.handle, &snapshot, &o, &report));
	alter_path = 1;
	CHECK(!wf_copy_source_observe(o, guard.handle, 1, &report));
	CHECK(ERROR_FILE_INVALID == report.error);
	alter_path = 0;
	CHECK(!wf_copy_source_finish(o, 1, 0, &report));
	wf_copy_source_release(o);
	o = NULL;
	CHECK(wf_copy_source_begin(guard.handle, &snapshot, &o, &report));
	fail_duplicate = 1;
	CHECK(!wf_copy_source_observe(o, guard.handle, 1, &report));
	CHECK(ERROR_NOT_ENOUGH_MEMORY == report.error);
	fail_duplicate = 0;
	CHECK(!wf_copy_source_finish(o, 1, 0, &report));
	wf_copy_source_release(o);
	o = NULL;
	CHECK(wf_copy_source_begin(guard.handle, &snapshot, &o, &report));
	o->report.callback_checks = UINT_MAX;
	CHECK(!wf_copy_source_observe(o, guard.handle, 1, &report));
	CHECK(ERROR_ARITHMETIC_OVERFLOW == report.error);
	wf_copy_source_release(o);
	o = NULL;
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) &&
	        before == after);
	p0_source_guard_release(&guard);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
