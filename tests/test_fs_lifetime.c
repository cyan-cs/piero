#include <windows.h>

#include "fs/wf_enum_win.h"
#include "wf_test.h"
static HANDLE entered, release_enum;
static WfStatus blocked_enum(const wchar_t *path,
        size_t limit,
        size_t bytes,
        WfSortKey key,
        bool descending,
        bool dirs_first,
        bool (*cancel)(void *),
        void *ctx,
        WfDir **out)
{
	(void)path;
	(void)limit;
	(void)bytes;
	(void)key;
	(void)descending;
	(void)dirs_first;
	(void)cancel;
	(void)ctx;
	*out = NULL;
	SetEvent(entered);
	WaitForSingleObject(release_enum, 5000);
	return WF_E_OS;
}
#define wf_enum_dir_win_ordered blocked_enum
#ifndef WF_FSWORKER_SOURCE
#	define WF_FSWORKER_SOURCE "../src/fs/wf_fsworker.c"
#endif
#include WF_FSWORKER_SOURCE
#undef wf_enum_dir_win_ordered
int main(void)
{
	DWORD before, after;
	entered = CreateEventW(NULL, TRUE, FALSE, NULL);
	release_enum = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(entered && release_enum);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (unsigned i = 0; i < 2; i++) {
		WfFsWorker *worker = NULL;
		WfFsPort port;
		HANDLE completion = NULL;
		ResetEvent(entered);
		ResetEvent(release_enum);
		CHECK(WF_OK == wf_fsworker_start(&worker));
		if (!worker) {
			break;
		}
		port = wf_fsworker_port(worker);
		CHECK(WF_OK ==
		        port.submit(port.ctx, i + 1, L"C:\\only-test", NULL));
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(entered, 3000));
		{
			WfWorkerStopReport report = wf_fsworker_stop(worker);
			CHECK(WF_E_TIMEOUT == report.status &&
			        ERROR_TIMEOUT == report.error &&
			        report.deferred);
			completion = report.completion;
			printf("listing stop status=%s Win32=%lu deferred=%d\n",
			        wf_status_name(report.status), report.error,
			        report.deferred);
			CHECK(NULL != completion);
		}
		worker = NULL;
		SetEvent(release_enum);
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(completion, 3000));
		CloseHandle(completion);
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
		CHECK(after == before);
	}
	CloseHandle(release_enum);
	CloseHandle(entered);
	return TEST_RESULT();
}
