#include "wf_workcore.h"

bool (*wf_wc_test_fail)(int step);

WfStatus wf_wc_start(WfWorkCore *c, LPTHREAD_START_ROUTINE fn, void *arg)
{
	c->references = 2;
	InitializeSRWLock(&c->lock);
	InitializeConditionVariable(&c->wake);
	c->ready = wf_wc_test_fail && wf_wc_test_fail(0)
	        ? NULL
	        : CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!c->ready) {
		return WF_E_OS;
	}
	c->thread = wf_wc_test_fail && wf_wc_test_fail(1)
	        ? NULL
	        : CreateThread(NULL, 0, fn, arg, 0, NULL);
	if (!c->thread) {
		CloseHandle(c->ready);
		c->ready = NULL;
		return WF_E_OS;
	}
	return WF_OK;
}

void wf_wc_release(WfWorkCore *c, void (*free_rest)(void *owner), void *owner)
{
	if (InterlockedDecrement(&c->references)) {
		return;
	}
	if (c->thread) {
		CloseHandle(c->thread);
	}
	CloseHandle(c->ready);
	free_rest(owner);
}

WfWorkerStopReport wf_wc_stop(WfWorkCore *c,
        DWORD wait_ms,
        bool cancel_io,
        void (*free_rest)(void *owner),
        void *owner)
{
	WfWorkerStopReport report = {0};
	DWORD wait;

	AcquireSRWLockExclusive(&c->lock);
	c->stop = true;
	if (cancel_io && c->busy && !CancelSynchronousIo(c->thread)) {
		report.cancel_error = GetLastError();
		if (ERROR_NOT_FOUND == report.cancel_error) {
			report.cancel_error = 0;
		}
	}
	report.stats = c->stats;
	ReleaseSRWLockExclusive(&c->lock);
	WakeConditionVariable(&c->wake);

	wait = WaitForSingleObject(c->thread, wait_ms);
	if (WAIT_OBJECT_0 != wait) {
		report.error =
		        WAIT_FAILED == wait ? GetLastError() : ERROR_TIMEOUT;
		report.status = WAIT_TIMEOUT == wait ? WF_E_TIMEOUT : WF_E_OS;
		report.deferred = 1;
		report.completion = c->thread;
		c->thread = NULL; /* handed over; the caller reference pins c */
	} else {
		AcquireSRWLockShared(&c->lock); /* the final counters */
		report.stats = c->stats;
		ReleaseSRWLockShared(&c->lock);
	}
	wf_wc_release(c, free_rest, owner);
	return report;
}
