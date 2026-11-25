#include <string.h>

#include "../tools/p0/p0_lab.h"
#include "../tools/p0/p0_source_guard.h"
#include "wf_test.h"
#include "xfer/wf_copy_source_win.h"
#include "xfer/wf_probe_win.h"
typedef struct
{
	WfCopySourceObserver *observer;
	WfCopySourceReport report;
} Context;
static DWORD CALLBACK progress(LARGE_INTEGER total,
        LARGE_INTEGER transferred,
        LARGE_INTEGER stream_size,
        LARGE_INTEGER stream_done,
        DWORD stream,
        DWORD reason,
        HANDLE source,
        HANDLE destination,
        void *context)
{
	Context *c = context;
	(void)total;
	(void)transferred;
	(void)stream_size;
	(void)stream_done;
	(void)reason;
	(void)destination;
	return wf_copy_source_observe(c->observer, source, stream, &c->report)
	        ? PROGRESS_CONTINUE
	        : PROGRESS_STOP;
}
static void make(P0Lab *lab, const wchar_t *leaf, DWORD size)
{
	wchar_t path[1024];
	char bytes[65536] = {0};
	DWORD written;
	HANDLE h;
	CHECK(p0_lab_path(lab, leaf, path, 1024));
	h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != h);
	CHECK(WriteFile(h, bytes, size, &written, NULL) && written == size);
	CloseHandle(h);
	CHECK(p0_area_track(lab->area, leaf, 0));
}
static void copy_case(P0Lab *lab,
        const wchar_t *guard_name,
        const wchar_t *source_name,
        const wchar_t *copy_name,
        int same)
{
	wchar_t source[1024], guarded[1024], destination[1024];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo snapshot;
	P0SourceGuard guard;
	Context c = {0};
	DWORD error, attributes;
	BOOL copied;
	CHECK(p0_lab_path(lab, guard_name, guarded, 1024));
	CHECK(p0_lab_path(lab, source_name, source, 1024));
	CHECK(p0_lab_path(lab, copy_name, destination, 1024));
	CHECK(WF_OK == ops.node(NULL, guarded, &snapshot));
	CHECK(P0_GUARD_OK ==
	        p0_source_guard_acquire(guarded, &snapshot, &guard, &error));
	CHECK(wf_copy_source_begin(
	        guard.handle, &snapshot, &c.observer, &c.report));
	if (c.observer) {
		copied = CopyFileExW(source, destination, progress, &c, NULL,
		        COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK);
		error = copied ? 0 : GetLastError();
		CHECK((0 != copied) == same);
		CHECK(wf_copy_source_finish(c.observer, 0 != copied, error,
		              &c.report) == same);
		CHECK(c.report.callback_checks > 0);
		CHECK(c.report.state ==
		        (same ? WF_COPY_SOURCE_ACCEPTED
		              : WF_COPY_SOURCE_REJECTED));
		printf("copy_source expected_match=%d accepted=%d "
		       "api_error=%lu checks=%u volume=%016llx\n",
		        same, WF_COPY_SOURCE_ACCEPTED == c.report.state, error,
		        c.report.callback_checks,
		        (unsigned long long)
		                c.report.observed.VolumeSerialNumber);
		wf_copy_source_release(c.observer);
	}
	p0_source_guard_release(&guard);
	attributes = GetFileAttributesW(destination);
	if (INVALID_FILE_ATTRIBUTES != attributes) {
		CHECK(p0_area_track(lab->area, copy_name, 0));
	} else {
		CHECK(ERROR_FILE_NOT_FOUND == GetLastError());
	}
}
/* Real pathname TOCTOU: change only this run's verified DOS alias after the
 * source guard opens. The CopyFileExW source string stays exactly the same.
 * Both possible targets are manifested fixtures with identical contents. */
static void alias_reresolution(P0Lab *lab)
{
	wchar_t source[1024], destination[1024], alternate[1200],
	        observed[1200];
	WfProbeOps ops = wf_probe_win();
	WfNodeInfo snapshot;
	P0SourceGuard guard;
	Context c = {0};
	DWORD error;
	int pushed = 0;
	BOOL copied;
	CHECK(p0_area_mkdir(lab->area, L"alternate"));
	make(lab, L"alternate\\source", 65536);
	CHECK(p0_lab_path(lab, L"source", source, 1024));
	CHECK(p0_area_path(
	        lab->area, L"alias-rejected-copy", destination, 1024));
	CHECK(WF_OK == ops.node(NULL, source, &snapshot));
	CHECK(P0_GUARD_OK ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	CHECK(wf_copy_source_begin(
	        guard.handle, &snapshot, &c.observer, &c.report));
	if (!c.observer) {
		p0_source_guard_release(&guard);
		return;
	}
	if (!QueryDosDeviceW(lab->drive, observed, 1200) ||
	        wcscmp(observed, lab->target)) {
		CHECK(0);
		goto done;
	}
	swprintf(alternate, 1200, L"%ls\\alternate", lab->target);
	if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM,
	            lab->drive, alternate)) {
		CHECK(0);
		goto done;
	}
	pushed = 1;
	CHECK(QueryDosDeviceW(lab->drive, observed, 1200) &&
	        !wcscmp(observed, alternate));
	copied = CopyFileExW(source, destination, progress, &c, NULL,
	        COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_COPY_SYMLINK);
	error = copied ? 0 : GetLastError();
	CHECK(!copied && ERROR_REQUEST_ABORTED == error);
	CHECK(!wf_copy_source_finish(
	        c.observer, 0 != copied, error, &c.report));
	CHECK(WF_COPY_SOURCE_REJECTED == c.report.state &&
	        c.report.callback_checks > 0);
	CHECK(memcmp(c.report.expected.FileId.Identifier,
	        c.report.observed.FileId.Identifier, 16));
	printf("real_alias_TOCTOU source_string_unchanged=1 api_error=%lu "
	       "accepted=0 expected_id=",
	        error);
	for (unsigned i = 0; i < 16; i++) {
		printf("%02x", c.report.expected.FileId.Identifier[i]);
	}
	printf(" observed_id=");
	for (unsigned i = 0; i < 16; i++) {
		printf("%02x", c.report.observed.FileId.Identifier[i]);
	}
	puts("");
done:
	if (pushed) {
		CHECK(DefineDosDeviceW(DDD_RAW_TARGET_PATH |
		                DDD_NO_BROADCAST_SYSTEM |
		                DDD_REMOVE_DEFINITION |
		                DDD_EXACT_MATCH_ON_REMOVE,
		        lab->drive, alternate));
		CHECK(QueryDosDeviceW(lab->drive, observed, 1200) &&
		        !wcscmp(observed, lab->target));
	}
	wf_copy_source_release(c.observer);
	p0_source_guard_release(&guard);
	if (INVALID_FILE_ATTRIBUTES != GetFileAttributesW(destination)) {
		CHECK(p0_area_track(lab->area, L"alias-rejected-copy", 0));
	} else {
		CHECK(ERROR_FILE_NOT_FOUND == GetLastError());
	}
}
int main(void)
{
	P0Lab lab;
	WfProbeOps ops = wf_probe_win();
	P0SourceGuard guard;
	WfNodeInfo snapshot;
	WfCopySourceObserver *observer = NULL;
	WfCopySourceReport report;
	wchar_t source[1024], different[1024];
	HANDLE wrong;
	DWORD error, before, after;
	CHECK(p0_lab_open(&lab, "copy-source", 1));
	if (!lab.area) {
		return 1;
	}
	make(&lab, L"source", 65536);
	make(&lab, L"different-identity-same-bytes", 65536);
	make(&lab, L"empty", 0);
	copy_case(&lab, L"source", L"source", L"copy", 1);
	copy_case(&lab, L"source", L"different-identity-same-bytes",
	        L"rejected-copy", 0);
	copy_case(&lab, L"empty", L"empty", L"empty-copy", 1);
	alias_reresolution(&lab);
	{
		wchar_t stream_path[1200];
		DWORD written;
		HANDLE stream;
		make(&lab, L"ads-source", 4);
		CHECK(p0_lab_path(&lab, L"ads-source", source, 1024));
		swprintf(stream_path, 1200, L"%ls:fixture", source);
		stream = CreateFileW(stream_path, GENERIC_WRITE, 0, NULL,
		        CREATE_NEW, 0, NULL);
		CHECK(INVALID_HANDLE_VALUE != stream &&
		        WriteFile(stream, "ADS", 3, &written, NULL) &&
		        3 == written);
		CloseHandle(stream);
		/* Base file and this explicitly created stream belong to the
		 * run. Unknown streams refuse; no promise of full-metadata
		 * verification. */
		copy_case(&lab, L"ads-source", L"ads-source",
		        L"ads-rejected-copy", 0);
	}
	CHECK(p0_lab_path(&lab, L"source", source, 1024));
	CHECK(p0_lab_path(
	        &lab, L"different-identity-same-bytes", different, 1024));
	CHECK(WF_OK == ops.node(NULL, source, &snapshot));
	CHECK(P0_GUARD_OK ==
	        p0_source_guard_acquire(source, &snapshot, &guard, &error));
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	{
		WfNodeInfo wrong_snapshot = snapshot;
		wrong_snapshot.id.id[0] ^= 1;
		CHECK(!wf_copy_source_begin(
		        guard.handle, &wrong_snapshot, &observer, &report));
		CHECK(NULL == observer);
		wrong_snapshot = snapshot;
		wrong_snapshot.id.volume ^= 1;
		CHECK(!wf_copy_source_begin(
		        guard.handle, &wrong_snapshot, &observer, &report));
		CHECK(NULL == observer);
	}
	CHECK(wf_copy_source_begin(
	        guard.handle, &snapshot, &observer, &report));
	if (observer) {
		CHECK(!wf_copy_source_finish(observer, 1, 0, &report));
		CHECK(WF_COPY_SOURCE_UNOBSERVED == report.state);
		wf_copy_source_release(observer);
		observer = NULL;
	}
	CHECK(wf_copy_source_begin(
	        guard.handle, &snapshot, &observer, &report));
	wrong = CreateFileW(different, GENERIC_READ, FILE_SHARE_READ, NULL,
	        OPEN_EXISTING, 0, NULL);
	if (observer) {
		CHECK(!wf_copy_source_observe(observer, wrong, 1, &report));
		CHECK(!wf_copy_source_observe(
		        observer, guard.handle, 1, &report));
		CHECK(!wf_copy_source_finish(observer, 1, 0, &report));
		wf_copy_source_release(observer);
		observer = NULL;
	}
	CloseHandle(wrong);
	CHECK(wf_copy_source_begin(
	        guard.handle, &snapshot, &observer, &report));
	if (observer) {
		CHECK(wf_copy_source_observe(
		        observer, guard.handle, 1, &report));
		CHECK(!wf_copy_source_finish(
		        observer, 0, ERROR_REQUEST_ABORTED, &report));
		CHECK(ERROR_REQUEST_ABORTED == report.error);
		wf_copy_source_release(observer);
		observer = NULL;
	}
	CHECK(wf_copy_source_begin(
	        guard.handle, &snapshot, &observer, &report));
	if (observer) {
		CHECK(!wf_copy_source_observe(
		        observer, guard.handle, 2, &report));
		CHECK(ERROR_NOT_SUPPORTED == report.error);
		CHECK(!wf_copy_source_finish(observer, 1, 0, &report));
		wf_copy_source_release(observer);
	}
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &after) &&
	        before == after);
	p0_source_guard_release(&guard);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
