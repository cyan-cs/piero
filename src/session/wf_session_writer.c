#include "../async/wf_workcore.h"
#include "wf_session_writer.h"

#include <stdlib.h>
#include <string.h>

struct WfSessionWriter
{
	WfWorkCore core;
	WfSessionStore *store; /* borrowed */
	char *pending; /* owned until replaced or dequeued; guarded by lock */
	size_t pending_n;
	volatile LONG failures; /* writes that failed, not yet told */
	volatile LONG written;
};

static void writer_free(void *owner)
{
	WfSessionWriter *w = owner;

	free(w->pending);
	free(w);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfSessionWriter *w = arg;

	for (;;) {
		char *text;
		size_t n;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->pending) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		/* what is pending is written even when a stop was asked: that
		 * is the last state of a normal exit */
		text = w->pending;
		n = w->pending_n;
		w->pending = NULL;
		w->pending_n = 0;
		if (text) {
			w->core.busy = true;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
		if (!text) {
			break; /* stop, and nothing left to write */
		}
		if (WF_OK == wf_session_store_write(w->store, text, n)) {
			InterlockedIncrement(&w->written);
		} else {
			InterlockedIncrement(&w->failures);
		}
		free(text);
		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, writer_free, w);
	return 0;
}

WfStatus wf_session_writer_start(WfSessionStore *store, WfSessionWriter **out)
{
	WfSessionWriter *w;

	*out = NULL;
	if (!wf_session_store_lock(store)) {
		return WF_E_ACCESS;
	}
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->store = store;
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfStatus wf_session_writer_submit(WfSessionWriter *w, char *text, size_t n)
{
	if (!w || !text) {
		free(text);
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(text);
		return WF_E_INVAL;
	}
	if (w->pending && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	free(w->pending);
	w->pending = text;
	w->pending_n = n;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

bool wf_session_writer_take_failure(WfSessionWriter *w)
{
	return w && InterlockedExchange(&w->failures, 0) > 0;
}

unsigned long wf_session_writer_written(const WfSessionWriter *w)
{
	return w ? (unsigned long)w->written : 0;
}

WfWorkerStopReport wf_session_writer_stop(WfSessionWriter *w, DWORD wait_ms)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, wait_ms, false, writer_free, w);
}
