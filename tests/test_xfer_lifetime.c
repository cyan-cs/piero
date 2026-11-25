#include <windows.h>

#include "wf_test.h"
static int fail_wait, fail_cancel;
static DWORD worker_wait(HANDLE handle, DWORD milliseconds)
{
	if (fail_wait && 1000 == milliseconds) {
		SetLastError(ERROR_INVALID_HANDLE);
		return WAIT_FAILED;
	}
	return WaitForSingleObject(handle, milliseconds);
}
static BOOL worker_cancel(HANDLE thread)
{
	if (fail_cancel) {
		SetLastError(ERROR_ACCESS_DENIED);
		return FALSE;
	}
	return CancelSynchronousIo(thread);
}
#define WaitForSingleObject worker_wait
#define CancelSynchronousIo worker_cancel
#ifndef WF_XFERWORKER_SOURCE
#	define WF_XFERWORKER_SOURCE "../src/xfer/wf_xferworker.c"
#endif
#include WF_XFERWORKER_SOURCE
/* the stop and its waits live in the shared core: compiled here, with the
 * same injected failures */
#ifndef WF_WORKCORE_SOURCE
#	define WF_WORKCORE_SOURCE "../src/async/wf_workcore.c"
#endif
#include WF_WORKCORE_SOURCE
#undef CancelSynchronousIo
#undef WaitForSingleObject

typedef struct
{
	HANDLE entered, release;
} Gate;
static WfStatus blocked_node(void *ctx, const wchar_t *path, WfNodeInfo *out)
{
	Gate *gate = ctx;
	(void)path;
	(void)out;
	SetEvent(gate->entered);
	WaitForSingleObject(gate->release, 5000);
	return WF_E_NOENT;
}
int main(void)
{
	Gate gate = {CreateEventW(NULL, TRUE, FALSE, NULL),
	        CreateEventW(NULL, TRUE, FALSE, NULL)};
	WfProbeOps ops = {0};
	const wchar_t *names[] = {L"file"};
	DWORD before, after;
	ops.ctx = &gate;
	ops.node = blocked_node;
	CHECK(gate.entered && gate.release);
	CHECK(GetProcessHandleCount(GetCurrentProcess(), &before));
	for (unsigned i = 0; i < 2; i++) {
		WfXferWorker *worker = NULL;
		HANDLE completion = NULL;
		WfXferPort port;
		ResetEvent(gate.entered);
		ResetEvent(gate.release);
		CHECK(WF_OK ==
		        wf_xferworker_start(
		                &worker, ops, WF_PREFLIGHT_DEFAULT));
		if (!worker) {
			break;
		}
		port = wf_xferworker_port(worker);
		CHECK(port.capture(port.ctx, i + 1, WF_XFER_COPY,
		              L"C:\\isolated", names, 1, 0) == WF_OK);
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(gate.entered, 3000));
		CHECK(port.capture(port.ctx, 100, WF_XFER_COPY, L"C:\\isolated",
		              names, 1, 0) == WF_OK);
		CHECK(port.capture(port.ctx, 101, WF_XFER_COPY, L"C:\\isolated",
		              names, 1, 0) == WF_OK);
		CHECK(1 == wf_xferworker_stats(worker).superseded_requests);
		fail_wait = fail_cancel = (1 == i);
		{
			WfWorkerStopReport report = wf_xferworker_stop(worker);
			CHECK(report.status == (i ? WF_E_OS : WF_E_TIMEOUT));
			CHECK(report.error ==
			        (DWORD)(i ? ERROR_INVALID_HANDLE
			                  : ERROR_TIMEOUT));
			CHECK(report.cancel_error ==
			        (DWORD)(i ? ERROR_ACCESS_DENIED : 0));
			CHECK(report.deferred);
			completion = report.completion;
			CHECK(NULL != completion);
			CHECK(1 == report.stats.superseded_requests);
			printf("xfer stop status=%s Win32=%lu cancel=%lu "
			       "deferred=%d "
			       "superseded=%llu\n",
			        wf_status_name(report.status), report.error,
			        report.cancel_error, report.deferred,
			        (unsigned long long)
			                report.stats.superseded_requests);
		}
		worker = NULL; /* stop consumes caller ownership even when I/O
		                  is stuck */
		SetEvent(gate.release);
		CHECK(WAIT_OBJECT_0 == WaitForSingleObject(completion, 3000));
		CloseHandle(completion);
		CHECK(GetProcessHandleCount(GetCurrentProcess(), &after));
		CHECK(after == before);
	}
	fail_wait = fail_cancel = 0;
	{
		WfXferWorker *worker = NULL;
		WfXferResult result;
		WfXferPort port;
		WfWorkerStats stats;
		unsigned received = 0;
		uint32_t newest = 0;
		CHECK(WF_OK ==
		        wf_xferworker_start(
		                &worker, ops, WF_PREFLIGHT_DEFAULT));
		if (worker) {
			port = wf_xferworker_port(worker);
			for (uint32_t gen = 1; gen <= 9; gen++) {
				ULONGLONG deadline = GetTickCount64() + 3000;
				CHECK(port.capture(port.ctx, gen, WF_XFER_COPY,
				              L"C:\\isolated", names, 1,
				              0) == WF_OK);
				while (wf_xferworker_stats(worker)
				                        .completed_requests <
				                gen &&
				        GetTickCount64() < deadline) {
					Sleep(1);
				}
				CHECK(wf_xferworker_stats(worker)
				                .completed_requests == gen);
			}
			stats = wf_xferworker_stats(worker);
			CHECK(9 == stats.completed_requests &&
			        stats.discarded_results > 0);
			while (wf_xferworker_poll(worker, &result)) {
				CHECK(result.gen > newest);
				newest = result.gen;
				received++;
				wf_slot_free(&result.slot);
			}
			CHECK(9 == newest &&
			        received + stats.discarded_results == 9);
			CHECK(WF_OK == wf_xferworker_stop(worker).status);
		}
		CHECK(WF_OK == wf_xferworker_stop(NULL).status);
	}
	CloseHandle(gate.release);
	CloseHandle(gate.entered);
	return TEST_RESULT();
}
