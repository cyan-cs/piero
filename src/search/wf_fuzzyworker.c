#include "../async/wf_workcore.h"
#include "wf_fuzzyworker.h"

#include <stdlib.h>
#include <string.h>

#define STOP_WAIT_MS 1000

struct WfFuzzyWorker
{
	WfWorkCore core;
	CONDITION_VARIABLE idle; /* signalled when a request has ended */

	bool has_req; /* a request waits for the thread */
	uint32_t req_gen;
	WfFzJob job;

	volatile LONG
	        live_gen; /* 0: none; read without the lock by the search */
	volatile LONG stopping;

	bool has_ans;
	WfFzAnswer ans;

	WfFzScratch *scratch;
};

static void worker_free(void *owner)
{
	WfFuzzyWorker *w = owner;

	wf_fz_answer_free(&w->ans);
	wf_fz_scratch_free(w->scratch);
	free(w);
}

/* The search asks this every 32 names: is this request still wanted? */
typedef struct
{
	const WfFuzzyWorker *w;
	LONG gen;
} Probe;

static bool probe_cancelled(void *ctx)
{
	const Probe *p = ctx;

	return p->w->stopping || p->gen != p->w->live_gen;
}

/* Runs one request outside the lock. Returns the answer (hit may be NULL). */
static WfFzAnswer run_job(WfFuzzyWorker *w, uint32_t gen, const WfFzJob *job)
{
	WfFzAnswer a = {0};
	WfFzTopK top;
	WfFzStats st = {0};
	Probe probe = {w, (LONG)gen};

	a.gen = gen;
	a.st = wf_fz_topk_init(&top, job->k);
	if (WF_OK != a.st) {
		return a;
	}
	a.st = wf_fz_search(&job->query, job->e, job->vis, job->nvis,
	        w->scratch, &top, probe_cancelled, &probe, &st);
	a.matched = st.matched;
	a.scanned = st.scanned;
	if (st.cancelled) {
		a.st = WF_E_CANCELLED;
	}
	a.hit = top.hit; /* moved to the answer */
	a.nhit = top.n;
	return a;
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfFuzzyWorker *w = arg;

	for (;;) {
		uint32_t gen;
		WfFzJob job;
		WfFzAnswer a;

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
		job = w->job;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		a = run_job(w, gen, &job);

		AcquireSRWLockExclusive(&w->core.lock);
		/* A request that is still the newest is answered whatever came of it: a
		 * failure is told to the screen too, or it would wait for an answer that
		 * never comes. Only an answer to a request that was replaced, cancelled
		 * or stopped is dropped. */
		if ((LONG)gen == w->live_gen && !w->stopping &&
		        WF_E_CANCELLED != a.st) {
			wf_fz_answer_free(&w->ans);
			w->ans = a;
			w->has_ans = true;
			SetEvent(w->core.ready);
		} else {
			wf_fz_answer_free(&a);
			if (UINT64_MAX != w->core.stats.discarded_results) {
				w->core.stats.discarded_results++;
			}
		}
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
		WakeAllConditionVariable(&w->idle);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_fuzzyworker_start(WfFuzzyWorker **out)
{
	WfFuzzyWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->scratch = wf_fz_scratch_new();
	InitializeConditionVariable(&w->idle);
	if (!w->scratch) {
		free(w);
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		wf_fz_scratch_free(w->scratch);
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_fuzzyworker_stop(WfFuzzyWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	InterlockedExchange(&w->stopping, 1);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

HANDLE wf_fuzzyworker_event(const WfFuzzyWorker *w)
{
	return w->core.ready;
}

bool wf_fuzzyworker_poll(WfFuzzyWorker *w, WfFzAnswer *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->has_ans) {
		*out = w->ans;
		memset(&w->ans, 0, sizeof w->ans);
		w->has_ans = false;
		got = true;
	} else {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

static WfStatus port_start(void *ctx, uint32_t gen, const WfFzJob *job)
{
	WfFuzzyWorker *w = ctx;

	if (!w || !job || !gen || !job->e || !job->vis || 0 == job->nvis ||
	        0 == job->query.n || 0 == job->k) {
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		return WF_E_INVAL;
	}
	if (w->has_req && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	w->job = *job;
	w->req_gen = gen;
	w->has_req = true;
	InterlockedExchange(&w->live_gen, (LONG)gen);
	wf_fz_answer_free(&w->ans);
	w->has_ans = false;
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static void port_release(void *ctx)
{
	WfFuzzyWorker *w = ctx;

	if (!w) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	InterlockedExchange(&w->live_gen, 0);
	w->has_req = false;
	wf_fz_answer_free(&w->ans);
	w->has_ans = false;
	ResetEvent(w->core.ready);
	/* the running search sees live_gen change within 32 names; it is pure
	 * computation, so this wait is short and is what keeps the borrowed
	 * listing alive for as long as it is read */
	while (w->core.busy) {
		SleepConditionVariableSRW(&w->idle, &w->core.lock, INFINITE, 0);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

WfFuzzyPort wf_fuzzyworker_port(WfFuzzyWorker *w)
{
	WfFuzzyPort p = {w, port_start, port_release};

	return p;
}
