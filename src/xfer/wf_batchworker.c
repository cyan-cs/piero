#include "../async/wf_workcore.h"
#include "wf_batchworker.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_batchexec_win.h"

/* A cancelled batch rolls back, which is one rename per step done: allow it
 * time before the thread is handed over as a deferred completion. */
#define STOP_WAIT_MS 60000

/* The screen's types are the engine's, renamed: a change on one side that is
 * not made on the other is a compile error, not a wrong word on a screen. */
_Static_assert((int)WF_BX_SUCCESS == (int)WF_BATCH_SUCCESS &&
                (int)WF_BX_ROLLED_BACK == (int)WF_BATCH_ROLLED_BACK &&
                (int)WF_BX_RECOVERY_REQUIRED ==
                        (int)WF_BATCH_RECOVERY_REQUIRED &&
                (int)WF_BX_FAILED_BEFORE ==
                        (int)WF_BATCH_FAILED_BEFORE_EXECUTION,
        "result values");
_Static_assert((int)WF_BX_PRE_READY == (int)WF_BATCH_READY &&
                (int)WF_BX_PRE_MISSING == (int)WF_BATCH_PRE_MISSING &&
                (int)WF_BX_PRE_CHANGED == (int)WF_BATCH_PRE_CHANGED &&
                (int)WF_BX_PRE_ACCESS == (int)WF_BATCH_PRE_ACCESS &&
                (int)WF_BX_PRE_PLAN == (int)WF_BATCH_PRE_PLAN &&
                (int)WF_BX_PRE_LIMITS == (int)WF_BATCH_PRE_LIMITS &&
                (int)WF_BX_PRE_UNSUPPORTED == (int)WF_BATCH_PRE_UNSUPPORTED &&
                (int)WF_BX_PRE_NOMEM == (int)WF_BATCH_PRE_NOMEM,
        "pre values");
_Static_assert(sizeof(WfBxSource) == sizeof(WfBatchSource) &&
                offsetof(WfBxSource, path) == offsetof(WfBatchSource, path) &&
                offsetof(WfBxSource, expected) ==
                        offsetof(WfBatchSource, expected) &&
                offsetof(WfBxSource, to_name) ==
                        offsetof(WfBatchSource, to_name),
        "source layout");

typedef enum
{
	REQ_NONE,
	REQ_RUN,
	REQ_RECOVER
} Req;

struct WfBatchWorker
{
	WfWorkCore core;
	wchar_t *dir;

	Req req;
	uint32_t req_gen;
	WfBatchJob *job; /* owned from run() until the request ends */

	volatile LONG cancel;   /* read by the engine before each step */
	volatile LONG progress; /* steps done, written by the engine's hook */

	bool has_result;
	WfBxDone result;
};

static void worker_free(void *owner)
{
	WfBatchWorker *w = owner;

	if (w->job) {
		wf_batch_job_free(w->job);
	}
	free(w->dir);
	free(w);
}

static void report_out(const WfBatchReport *r, WfBxReport *o)
{
	memset(o, 0, sizeof *o);
	o->result = (WfBxResult)r->result;
	o->pre = (WfBxPre)r->pre;
	o->pre_item = r->pre_item;
	o->steps_total = r->steps_total;
	o->steps_done = r->steps_done;
	o->rolled_back = r->rolled_back;
	o->not_restored = r->not_restored;
	o->failed_step = r->failed_step;
	o->error = r->error;
	o->calls.opens = r->calls.opens;
	o->calls.id_queries = r->calls.id_queries;
	o->calls.renames = r->calls.renames;
	o->calls.enumerations = r->calls.enumerations;
	o->log_writes = r->log.writes;
	o->log_flushes = r->log.flushes;
	o->ms_prepare = r->ms_prepare;
	o->ms_journal = r->ms_journal;
	o->ms_rename = r->ms_rename;
	o->ms_rollback = r->ms_rollback;
}

/* The engine's hook: the step that has just been done and checked. */
static void hook_at(void *ctx, const char *point, size_t step)
{
	WfBatchWorker *w = ctx;

	if (!strcmp(point, "after_D")) {
		InterlockedExchange(&w->progress, (LONG)(step + 1));
	}
}

/* The journals that are unfinished or damaged are recovered, one by one; a
 * finished one (a receipt) and one without a plan are left alone, exactly the
 * set wf_batch_count_unfinished counts. */
static void recover_all(WfBatchWorker *w, WfBxDone *d)
{
	wchar_t *pattern = NULL;
	WIN32_FIND_DATAW data;
	HANDLE find;

	if (WF_OK != wf_path_join(w->dir, L"*.pbj", &pattern)) {
		d->rep.result = WF_BX_FAILED_BEFORE;
		return;
	}
	find = FindFirstFileExW(pattern, FindExInfoBasic, &data,
	        FindExSearchNameMatch, NULL, 0);
	free(pattern);
	d->rep.result = WF_BX_SUCCESS;
	if (INVALID_HANDLE_VALUE == find) {
		return;
	}
	do {
		wchar_t *path = NULL;
		WfBatchJournal j;
		WfBjReadResult rr;
		WfBatchReport rep;
		bool todo;

		if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
		        WF_OK != wf_path_join(w->dir, data.cFileName, &path)) {
			continue;
		}
		rr = wf_batchlog_read(path, &j);
		todo = WF_BJ_READ_NO_PLAN != rr;
		if (WF_BJ_READ_OK == rr) {
			todo = !j.ended || WF_BJ_RECOVERY_REQUIRED == j.end;
			wf_batchjournal_free(&j);
		}
		if (!todo) {
			free(path);
			continue;
		}
		wf_batch_recover(path, NULL, &rep);
		free(path);
		d->rec_total++;
		d->rep.rolled_back += rep.rolled_back;
		d->rep.not_restored += rep.not_restored;
		d->rep.steps_done += rep.steps_done;
		switch (rep.result) {
		case WF_BATCH_SUCCESS:     d->rec_done++; break;
		case WF_BATCH_ROLLED_BACK: d->rec_rolled_back++; break;
		default:
			d->rec_open++;
			d->rep.error = rep.error;
			break;
		}
	} while (FindNextFileW(find, &data));
	FindClose(find);
	if (d->rec_open) {
		d->rep.result = WF_BX_RECOVERY_REQUIRED;
	} else if (d->rec_rolled_back) {
		d->rep.result = WF_BX_ROLLED_BACK;
	}
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfBatchWorker *w = arg;

	for (;;) {
		Req req;
		uint32_t gen;
		WfBatchJob *job;
		WfBxDone d;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && REQ_NONE == w->req) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			/* a request that never began changed nothing: it is
			 * dropped (the job is freed with the worker) */
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		req = w->req;
		gen = w->req_gen;
		job = w->job;
		w->req = REQ_NONE;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		memset(&d, 0, sizeof d);
		d.gen = gen;
		if (REQ_RUN == req) {
			WfBatchReport rep;
			WfBatchHooks hooks = {hook_at, w};

			memset(&rep, 0, sizeof rep);
			wf_batch_execute(job, w->dir, &hooks, &w->cancel, &rep);
			report_out(&rep, &d.rep);
			wf_batch_job_free(job);
		} else {
			d.recover = true;
			recover_all(w, &d);
		}

		AcquireSRWLockExclusive(&w->core.lock);
		w->job = NULL;
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		w->result = d;
		w->has_result = true;
		SetEvent(w->core.ready);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_batchworker_start(WfBatchWorker **out, const wchar_t *journal_dir)
{
	WfBatchWorker *w;

	if (!out || !journal_dir || !journal_dir[0]) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->dir = wf_path_dup(journal_dir);
	if (!w->dir) {
		free(w);
		return WF_E_NOMEM;
	}
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w->dir);
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_batchworker_stop(WfBatchWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	InterlockedExchange(&w->cancel, 1);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

HANDLE wf_batchworker_event(const WfBatchWorker *w)
{
	return w->core.ready;
}

bool wf_batchworker_poll(WfBatchWorker *w, WfBxDone *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->has_result) {
		*out = w->result;
		w->has_result = false;
		got = true;
	}
	ResetEvent(w->core.ready);
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

static struct WfBatchJob *port_prepare(
        void *ctx, const WfBxSource *src, size_t n, WfBxReport *rep)
{
	WfBatchReport r;
	WfBatchJob *job;

	(void)ctx;
	job = wf_batch_prepare((const WfBatchSource *)src, n, NULL, &r);
	report_out(&r, rep);
	return job;
}

static void port_job_free(void *ctx, struct WfBatchJob *job)
{
	(void)ctx;
	wf_batch_job_free(job);
}

static bool port_ready(void *ctx, const struct WfBatchJob *job)
{
	(void)ctx;
	return wf_batch_job_ready(job);
}

static const WfBatchPlan *port_plan(void *ctx, const struct WfBatchJob *job)
{
	(void)ctx;
	return wf_batch_job_plan(job);
}

static WfStatus submit(WfBatchWorker *w, Req req, uint32_t gen, WfBatchJob *job)
{
	if (!w || !gen) {
		return WF_E_INVAL;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || REQ_NONE != w->req || w->core.busy ||
	        w->has_result) {
		ReleaseSRWLockExclusive(&w->core.lock);
		return WF_E_INVAL; /* one request at a time; never superseded */
	}
	InterlockedExchange(&w->cancel, 0);
	InterlockedExchange(&w->progress, 0);
	w->job = job;
	w->req_gen = gen;
	w->req = req;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static WfStatus port_run(void *ctx, uint32_t gen, struct WfBatchJob *job)
{
	if (!job || !wf_batch_job_ready(job)) {
		return WF_E_INVAL;
	}
	return submit(ctx, REQ_RUN, gen, job);
}

static WfStatus port_recover(void *ctx, uint32_t gen)
{
	return submit(ctx, REQ_RECOVER, gen, NULL);
}

static void port_cancel(void *ctx)
{
	WfBatchWorker *w = ctx;

	if (w) {
		InterlockedExchange(&w->cancel, 1);
	}
}

static size_t port_progress(void *ctx)
{
	WfBatchWorker *w = ctx;

	return w ? (size_t)w->progress : 0;
}

static size_t port_unfinished(void *ctx)
{
	WfBatchWorker *w = ctx;

	return w ? wf_batch_count_unfinished(w->dir) : 0;
}

WfBatchPort wf_batchworker_port(WfBatchWorker *w)
{
	WfBatchPort p = {w, port_prepare, port_job_free, port_ready, port_plan,
	        port_run, port_recover, port_cancel, port_progress,
	        port_unfinished};

	return p;
}
