#include <windows.h>

#include "../tools/p0/p0_lab.h"
#include "wf_test.h"
#include "wf_final_path_fault.h"
#include "async/wf_workcore.h"

static DWORD stop_wait_ms = 1000;

static WfWorkerStopReport lifetime_stop(WfWorkCore *core,
        DWORD wait_ms,
        bool cancel_io,
        void (*free_rest)(void *),
        void *owner)
{
	(void)wait_ms;
	return wf_wc_stop(core, stop_wait_ms, cancel_io, free_rest, owner);
}

static HANDLE entered, release_read;
static int fail_immediately;
static BOOL blocked_read(HANDLE file,
        LPVOID data,
        DWORD size,
        LPDWORD got,
        LPOVERLAPPED overlapped)
{
	(void)file;
	(void)data;
	(void)size;
	(void)overlapped;
	if (fail_immediately) {
		*got = 0;
		SetLastError(ERROR_CRC);
		return FALSE;
	}
	SetEvent(entered);
	WaitForSingleObject(release_read, 5000);
	*got = 0;
	SetLastError(ERROR_OPERATION_ABORTED);
	return FALSE;
}
#define ReadFile blocked_read
#define wf_wc_stop lifetime_stop
#define GetFinalPathNameByHandleW fault_final_path
#ifndef WF_PREVIEWWORKER_SOURCE
#	define WF_PREVIEWWORKER_SOURCE "../src/preview/wf_previewworker.c"
#endif
#include WF_PREVIEWWORKER_SOURCE
#undef ReadFile
#undef wf_wc_stop
#undef GetFinalPathNameByHandleW

static void final_path_failures_are_refused(void)
{
	Refusal refusal = {DEFAULT_REFUSAL, 0};

	final_path_fault = FINAL_PATH_OK;
	CHECK(require_guid_volume(INVALID_HANDLE_VALUE, &refusal));
	final_path_fault = FINAL_PATH_QUERY_FAILED;
	CHECK(!require_guid_volume(INVALID_HANDLE_VALUE, &refusal));
	CHECK(refusal.error == ERROR_ACCESS_DENIED);
	final_path_fault = FINAL_PATH_READ_FAILED;
	CHECK(!require_guid_volume(INVALID_HANDLE_VALUE, &refusal));
	CHECK(refusal.error == ERROR_CRC);
	final_path_fault = FINAL_PATH_GREW;
	CHECK(!require_guid_volume(INVALID_HANDLE_VALUE, &refusal));
	CHECK(refusal.error == ERROR_FILENAME_EXCED_RANGE);
	final_path_fault = FINAL_PATH_REAL;
}

int main(void)
{
	final_path_failures_are_refused();
	P0Lab lab;
	wchar_t path[1024];
	HANDLE file;
	DWORD before, after;
	CHECK(p0_lab_open(&lab, "preview-lifetime", 1));
	if (!lab.area) {
		return 1;
	}
	CHECK(p0_lab_path(&lab, L"only-test.txt", path, 1024));
	file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
	CHECK(INVALID_HANDLE_VALUE != file);
	if (INVALID_HANDLE_VALUE != file) {
		CloseHandle(file);
	}
	CHECK(p0_area_track(lab.area, L"only-test.txt", 0));
	entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	release_read = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(entered && release_read);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (unsigned i = 0; i < 102; i++) {
		WfPreviewWorker *worker = NULL;
		WfPreviewPort port;
		HANDLE completion = NULL;
		/* First exercise the product's deadline; subsequent turns exercise
		 * the same deferred ownership transfer with a short test deadline. */
		stop_wait_ms = i < 2 ? 1000 : 1;
		ResetEvent(entered);
		ResetEvent(release_read);
		CHECK(WF_OK ==
		        wf_previewworker_start(&worker, WF_PREVIEW_BYTES, 932));
		if (!worker) {
			break;
		}
		port = wf_previewworker_port(worker);
		CHECK(WF_OK == port.submit(port.ctx, i + 1, path, 0));
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(entered, 3000));
		{
			WfWorkerStopReport report =
			        wf_previewworker_stop(worker);
			CHECK(WF_E_TIMEOUT == report.status &&
			        ERROR_TIMEOUT == report.error &&
			        report.deferred);
			completion = report.completion;
			if (i < 2) {
				printf("preview stop status=%s Win32=%lu "
				       "deferred=%d\n",
				        wf_status_name(report.status),
				        report.error, report.deferred);
			}
			CHECK(NULL != completion);
		}
		worker = NULL;
		SetEvent(release_read);
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(completion, 3000));
		CloseHandle(completion);
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
		if (after != before) {
			printf("preview deferred cycle=%u handles=%lu -> %lu\n",
			        i, before, after);
		}
		CHECK(after == before);
	}
	{
		WfPreviewWorker *worker = NULL;
		WfPreviewResult result = {0};
		WfPreviewPort port;
		fail_immediately = 1;
		stop_wait_ms = 1000;
		CHECK(WF_OK ==
		        wf_previewworker_start(&worker, WF_PREVIEW_BYTES, 932));
		if (worker) {
			port = wf_previewworker_port(worker);
			CHECK(WF_OK == port.submit(port.ctx, 50, path, 0));
			CHECK(WaitForSingleObject(
			              wf_previewworker_event(worker), 3000) ==
			        WAIT_OBJECT_0);
			CHECK(wf_previewworker_poll(worker, &result));
			CHECK(result.preview &&
			        WF_PREVIEW_REFUSED == result.preview->kind &&
			        ERROR_CRC == result.preview->system_error);
			if (result.preview) {
				printf("preview read fault Win32=%lu kind=%d\n",
				        (unsigned long)
				                result.preview->system_error,
				        result.preview->kind);
			}
			wf_preview_free(result.preview);
			WfWorkerStopReport report =
			        wf_previewworker_stop(worker);
			CHECK(WF_OK == report.status);
			if (report.completion) {
				CloseHandle(report.completion);
			}
		}
	}
	CloseHandle(release_read);
	CloseHandle(entered);
	CHECK(0 == p0_lab_close(&lab, 0 != wf_test_failures));
	return TEST_RESULT();
}
