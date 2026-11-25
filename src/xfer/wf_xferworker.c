#include "wf_xferworker.h"

#include <stdlib.h>
#include <string.h>

#include "../async/wf_workcore.h"
#include "../path/wf_path.h"

#define RESULT_CAP 4
#define STOP_WAIT_MS 1000

typedef struct
{
	bool present;
	WfXferResultKind kind;
	uint32_t gen;
	/* capture */
	WfTransferMode mode;
	wchar_t *dir;
	wchar_t **names;
	size_t n, hidden;
	/* preflight */
	WfTransferSlot slot;
	wchar_t *dest;
} Job;

struct WfXferWorker
{
	WfProbeOps ops;
	WfPreflightPolicy policy;
	WfWorkCore core;
	LONG cancel; /* synchronized only by Interlocked operations */
	Job job;     /* the one queued job */
	WfXferResult q[RESULT_CAP];
	size_t qhead, qn;
};

static void job_free(Job *j)
{
	size_t i;

	free(j->dir);
	for (i = 0; i < j->n; i++) {
		free(j->names[i]);
	}
	free(j->names);
	wf_slot_free(&j->slot);
	free(j->dest);
	memset(j, 0, sizeof *j);
}

static void result_free(WfXferResult *r)
{
	wf_slot_free(&r->slot);
}

static void worker_free(void *owner)
{
	WfXferWorker *w = owner;

	for (size_t i = 0; i < w->qn; i++) {
		result_free(&w->q[(w->qhead + i) % RESULT_CAP]);
	}
	job_free(&w->job);
	free(w);
}

/* Caller holds the lock. */
static void push_result(WfXferWorker *w, WfXferResult *r)
{
	if (RESULT_CAP == w->qn) {
		result_free(&w->q[w->qhead]);
		w->qhead = (w->qhead + 1) % RESULT_CAP;
		w->qn--;
		if (UINT64_MAX != w->core.stats.discarded_results) {
			w->core.stats.discarded_results++;
		}
	}
	w->q[(w->qhead + w->qn++) % RESULT_CAP] = *r;
	SetEvent(w->core.ready);
}

static bool cancel_set(void *ctx)
{
	return InterlockedCompareExchange(
	               &((WfXferWorker *)ctx)->cancel, 0, 0) != 0;
}

static void run_job(WfXferWorker *w, Job *j, WfXferResult *r)
{
	r->kind = j->kind;
	r->gen = j->gen;
	wf_slot_init(&r->slot);
	memset(&r->report, 0, sizeof r->report);

	if (WF_XR_CAPTURE == j->kind) {
		r->st = wf_xfer_capture(&w->ops, j->mode, j->dir,
		        (const wchar_t *const *)j->names, j->n, j->hidden,
		        &r->slot);
	} else {
		WfCancel c = {w, cancel_set};

		r->st = wf_preflight_run(
		        &w->ops, &j->slot, j->dest, &w->policy, &c, &r->report);
	}
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfXferWorker *w = arg;

	for (;;) {
		Job job;
		WfXferResult r;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->job.present) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		job = w->job;
		memset(&w->job, 0, sizeof w->job);
		InterlockedExchange(&w->cancel, 0);
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		run_job(w, &job, &r);
		job_free(&job);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		if (!w->core.stop) {
			push_result(w, &r);
		} else {
			result_free(&r);
			if (UINT64_MAX != w->core.stats.discarded_results) {
				w->core.stats.discarded_results++;
			}
		}
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_xferworker_start(
        WfXferWorker **out, WfProbeOps ops, WfPreflightPolicy policy)
{
	WfXferWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->ops = ops;
	w->policy = policy;
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_xferworker_stop(WfXferWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	/* stop and cancel are set under one hold of the lock: the thread cannot
	 * pop a job, and clear the flag, in between */
	AcquireSRWLockExclusive(&w->core.lock);
	w->core.stop = true;
	InterlockedExchange(&w->cancel, 1);
	ReleaseSRWLockExclusive(&w->core.lock);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

WfWorkerStats wf_xferworker_stats(WfXferWorker *w)
{
	WfWorkerStats stats = {0};

	if (!w) {
		return stats;
	}
	AcquireSRWLockShared(&w->core.lock);
	stats = w->core.stats;
	ReleaseSRWLockShared(&w->core.lock);
	return stats;
}

HANDLE wf_xferworker_event(const WfXferWorker *w)
{
	return w->core.ready;
}

bool wf_xferworker_poll(WfXferWorker *w, WfXferResult *out)
{
	bool got = false;

	if (!w || !out) {
		return 0;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->qn > 0) {
		*out = w->q[w->qhead];
		w->qhead = (w->qhead + 1) % RESULT_CAP;
		w->qn--;
		got = true;
	}
	if (0 == w->qn) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

/* Installs `j` as the queued job, superseding anything older. Takes ownership
 * of j's contents on success AND failure. */
static WfStatus enqueue_take(WfXferWorker *w, Job *j)
{
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		job_free(j);
		return WF_E_INVAL;
	}
	if (w->job.present && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	job_free(&w->job);
	w->job = *j;
	memset(j, 0, sizeof *j);
	w->job.present = true;
	if (w->core.busy) {
		InterlockedExchange(&w->cancel, 1);
		CancelSynchronousIo(w->core.thread);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static WfStatus submit_capture(void *ctx,
        uint32_t gen,
        WfTransferMode mode,
        const wchar_t *dir,
        const wchar_t *const *names,
        size_t n,
        size_t hidden)
{
	Job j;
	size_t i;

	if (!ctx || !dir || !names || 0 == n) {
		return WF_E_INVAL;
	}
	for (i = 0; i < n; i++) {
		if (!names[i]) {
			return WF_E_INVAL;
		}
	}
	memset(&j, 0, sizeof j);
	j.kind = WF_XR_CAPTURE;
	j.gen = gen;
	j.mode = mode;
	j.hidden = hidden;
	j.dir = wf_path_dup(dir);
	j.names = calloc(n, sizeof *j.names);
	if (!j.dir || !j.names) {
		goto nomem;
	}
	for (i = 0; i < n; i++) {
		if (!(j.names[i] = wf_path_dup(names[i]))) {
			goto nomem;
		}
		j.n = i + 1;
	}
	return enqueue_take(ctx, &j);
nomem:
	job_free(&j);
	return WF_E_NOMEM;
}

static WfStatus submit_preflight(void *ctx,
        uint32_t gen,
        const WfTransferSlot *slot,
        const wchar_t *dest)
{
	Job j;
	WfStatus st;

	if (!ctx || !slot || !dest) {
		return WF_E_INVAL;
	}
	memset(&j, 0, sizeof j);
	j.kind = WF_XR_PREFLIGHT;
	j.gen = gen;
	if (WF_OK != (st = wf_slot_clone(&j.slot, slot))) {
		return st;
	}
	if (!(j.dest = wf_path_dup(dest))) {
		job_free(&j);
		return WF_E_NOMEM;
	}
	return enqueue_take(ctx, &j);
}

WfXferPort wf_xferworker_port(WfXferWorker *w)
{
	WfXferPort port = {w, submit_capture, submit_preflight};

	return port;
}
