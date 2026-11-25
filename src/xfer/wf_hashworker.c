#include "../async/wf_workcore.h"
#include "wf_hashworker.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_sha256_win.h"

#define STOP_WAIT_MS 1000
#define PROGRESS_MS 100u

struct WfHashWorker
{
	WfWorkCore core;

	bool has_req; /* a request waits for the thread */
	uint32_t req_gen;
	wchar_t *req_path;

	volatile LONG live_gen; /* 0: none; read without the lock by the hash */
	volatile LONG stopping;

	bool has_upd;
	WfHashUpdate upd; /* the newest; an end is never replaced by progress */
};

static void worker_free(void *owner)
{
	WfHashWorker *w = owner;

	free(w->req_path);
	free(w);
}

typedef struct
{
	WfHashWorker *w;
	uint32_t gen;
	ULONGLONG last;
} Run;

static bool run_cancelled(void *ctx)
{
	const Run *r = ctx;

	return r->w->stopping || (LONG)r->gen != r->w->live_gen;
}

static void publish(WfHashWorker *w, const WfHashUpdate *u)
{
	AcquireSRWLockExclusive(&w->core.lock);
	if ((LONG)u->gen == w->live_gen && !w->stopping &&
	        !(w->has_upd && w->upd.finished && w->upd.gen == u->gen &&
	                !u->finished)) {
		w->upd = *u;
		w->has_upd = true;
		SetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

static void run_progress(void *ctx, uint64_t done, uint64_t total)
{
	Run *r = ctx;
	ULONGLONG now = GetTickCount64();
	WfHashUpdate u;

	if (now - r->last < PROGRESS_MS) {
		return;
	}
	r->last = now;
	memset(&u, 0, sizeof u);
	u.gen = r->gen;
	u.done = done;
	u.total = total;
	publish(r->w, &u);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfHashWorker *w = arg;

	for (;;) {
		uint32_t gen;
		wchar_t *path;
		Run run;
		WfHashUpdate end;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->has_req) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		gen = w->req_gen;
		path = w->req_path;
		w->req_path = NULL;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		run.w = w;
		run.gen = gen;
		run.last = GetTickCount64();
		memset(&end, 0, sizeof end);
		end.gen = gen;
		end.finished = true;
		wf_sha256_file(path, run_cancelled, &run, run_progress, &run,
		        &end.res);
		end.done = end.res.size;
		end.total = end.res.size;
		free(path);
		if (WF_E_CANCELLED != end.res.st) {
			publish(w, &end);
		}

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_hashworker_start(WfHashWorker **out)
{
	WfHashWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_hashworker_stop(WfHashWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	InterlockedExchange(&w->stopping, 1);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

HANDLE wf_hashworker_event(const WfHashWorker *w)
{
	return w->core.ready;
}

bool wf_hashworker_poll(WfHashWorker *w, WfHashUpdate *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->has_upd) {
		*out = w->upd;
		w->has_upd = false;
		got = true;
	}
	if (!w->has_upd) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

bool wf_hashworker_idle(WfHashWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->has_req && !w->has_upd;
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

static WfStatus port_start(void *ctx, uint32_t gen, const wchar_t *path)
{
	WfHashWorker *w = ctx;
	wchar_t *copy;

	if (!w || !path || !gen) {
		return WF_E_INVAL;
	}
	copy = wf_path_dup(path);
	if (!copy) {
		return WF_E_NOMEM;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(copy);
		return WF_E_INVAL;
	}
	if (w->has_req && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	free(w->req_path);
	w->req_path = copy;
	w->req_gen = gen;
	w->has_req = true;
	w->has_upd = false;
	InterlockedExchange(&w->live_gen, (LONG)gen);
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static void port_cancel(void *ctx)
{
	WfHashWorker *w = ctx;

	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	InterlockedExchange(&w->live_gen, 0);
	free(w->req_path);
	w->req_path = NULL;
	w->has_req = false;
	w->has_upd = false;
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
}

WfHashPort wf_hashworker_port(WfHashWorker *w)
{
	WfHashPort p = {w, port_start, port_cancel};

	return p;
}
