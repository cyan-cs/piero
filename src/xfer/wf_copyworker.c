#include "wf_copyworker.h"
#include "wf_extract_win.h"
#include "../async/wf_workcore.h"
#include "../base/wf_macro.h"

#include <stdlib.h>
#include <string.h>

#define STOP_WAIT_MS 15000
#define RESULT_MAX 16

typedef struct
{
	WfCopySpec spec;
	uint32_t id;
	bool cancelled; /* never starts: reports as cancelled when its turn comes */
} Job;

struct WfCopyWorker
{
	WfWorkCore core;
	WfCopyJournal *journal;

	Job queue[WF_COPY_QUEUE_MAX];
	size_t queued;
	uint32_t next_id;

	bool running;
	bool paused;
	bool cancel; /* the running job */
	WfCopyProgress progress;

	WfCopyResult results[RESULT_MAX];
	size_t nresults;
};

static void worker_free(void *owner)
{
	WfCopyWorker *w = owner;
	size_t i;

	for (i = 0; i < w->queued; i++) {
		wf_copyspec_free(&w->queue[i].spec);
	}
	for (i = 0; i < w->nresults; i++) {
		wf_copyresult_free(&w->results[i]);
	}
	wf_cj_close(w->journal);
	free(w);
}

static void worker_release(WfCopyWorker *w)
{
	wf_wc_release(&w->core, worker_free, w);
}

static bool checkpoint(void *ctx)
{
	WfCopyWorker *w = ctx;
	bool go;

	AcquireSRWLockExclusive(&w->core.lock);
	while (w->paused && !w->cancel && !w->core.stop) {
		w->progress.state = WF_CS_PAUSED;
		SleepConditionVariableSRW(&w->core.wake, &w->core.lock, 200, 0);
	}
	if (WF_CS_PAUSED == w->progress.state) {
		w->progress.state = WF_CS_RUNNING;
	}
	go = !w->cancel && !w->core.stop;
	ReleaseSRWLockExclusive(&w->core.lock);
	return go;
}

static void progress(
        void *ctx, size_t files, uint64_t bytes, const wchar_t *current)
{
	WfCopyWorker *w = ctx;

	AcquireSRWLockExclusive(&w->core.lock);
	w->progress.files_done = files;
	w->progress.bytes_done = bytes;
	if (current) {
		wcsncpy_s(w->progress.current,
		        WF_ARRAY_LEN(w->progress.current), current, _TRUNCATE);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

static void deliver(WfCopyWorker *w, WfCopyResult *r)
{
	if (RESULT_MAX == w->nresults) {
		/* not reachable: every caller makes room first (a job starts, or
		 * reports as cancelled, only with room for its result). Were it
		 * reached, the oldest unread one would have to go. */
		wf_copyresult_free(&w->results[0]);
		memmove(&w->results[0], &w->results[1],
		        (RESULT_MAX - 1) * sizeof w->results[0]);
		w->nresults--;
	}
	w->results[w->nresults++] = *r;
	memset(r, 0, sizeof *r);
	SetEvent(w->core.ready);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfCopyWorker *w = arg;
	WfCopyJournal *journal = w->journal;
	WfCopyControl ctl;

	ctl.ctx = w;
	ctl.checkpoint = checkpoint;
	ctl.progress = progress;
	for (;;) {
		Job job;
		WfCopyResult r;
		WfStatus st;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->queued) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		/* A job's result is never dropped: the next job does not start
		 * while the results nobody has read yet fill the store. */
		while (!w->core.stop && w->nresults >= RESULT_MAX) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, 100, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		job = w->queue[0];
		memmove(&w->queue[0], &w->queue[1],
		        (w->queued - 1) * sizeof w->queue[0]);
		w->queued--;
		if (job.cancelled) {
			memset(&r, 0, sizeof r);
			r.job = job.id;
			r.kind = job.spec.kind;
			r.state = WF_CS_CANCELLED;
			wf_copyspec_free(&job.spec);
			deliver(w, &r);
			ReleaseSRWLockExclusive(&w->core.lock);
			continue;
		}
		w->running = true;
		w->cancel = false;
		memset(&w->progress, 0, sizeof w->progress);
		w->progress.state = WF_CS_RUNNING;
		w->progress.job = job.id;
		w->progress.files_total = job.spec.total_files;
		w->progress.bytes_total = job.spec.total_bytes;
		ReleaseSRWLockExclusive(&w->core.lock);

		memset(&r, 0, sizeof r);
		st = WF_CJ_EXTRACT == job.spec.kind
		        ? wf_extract_run(&job.spec, &ctl, journal, &r)
		        : wf_copyengine_run(&job.spec, &ctl, journal, &r);
		if (WF_OK != st) {
			r.state = WF_CS_FAILED;
			r.kind = job.spec.kind;
		}
		r.job = job.id;
		wf_copyspec_free(&job.spec);

		AcquireSRWLockExclusive(&w->core.lock);
		w->running = false;
		w->progress.state = WF_CS_IDLE;
		deliver(w, &r);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	worker_release(w);
	return 0;
}

WfStatus wf_copyworker_start(WfCopyWorker **out, const wchar_t *journal_path)
{
	WfCopyWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->next_id = 1;
	/* Publish readiness only after the required receipt writer is valid.
	 * A thread must never discover startup failure after start returned OK. */
	w->journal = wf_cj_open(journal_path);
	if (!w->journal) {
		free(w);
		return WF_E_OS;
	}
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		wf_cj_close(w->journal);
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_copyworker_stop(WfCopyWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	/* stop and cancel together: the worker never starts one more job in
	 * between */
	AcquireSRWLockExclusive(&w->core.lock);
	w->core.stop = true;
	w->cancel = true;
	w->paused = false;
	ReleaseSRWLockExclusive(&w->core.lock);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

WfStatus wf_copyworker_submit(
        WfCopyWorker *w, const WfCopySpec *spec, uint32_t *job)
{
	WfStatus st;
	Job j;

	if (!w || !spec) {
		return WF_E_INVAL;
	}
	st = wf_copyspec_clone(&j.spec, spec);
	if (WF_OK != st) {
		return st;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || WF_COPY_QUEUE_MAX == w->queued) {
		ReleaseSRWLockExclusive(&w->core.lock);
		wf_copyspec_free(&j.spec);
		return WF_E_RANGE;
	}
	j.id = w->next_id++;
	j.cancelled = false;
	if (job) {
		*job = j.id;
	}
	w->queue[w->queued++] = j;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeAllConditionVariable(&w->core.wake);
	return WF_OK;
}

void wf_copyworker_set_paused(WfCopyWorker *w, bool paused)
{
	AcquireSRWLockExclusive(&w->core.lock);
	w->paused = paused;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeAllConditionVariable(&w->core.wake);
}

bool wf_copyworker_paused(WfCopyWorker *w)
{
	bool p;

	AcquireSRWLockShared(&w->core.lock);
	p = w->paused;
	ReleaseSRWLockShared(&w->core.lock);
	return p;
}

void wf_copyworker_cancel(WfCopyWorker *w, bool all)
{
	AcquireSRWLockExclusive(&w->core.lock);
	w->cancel = true;
	w->paused = false;
	if (all) {
		/* queued jobs never start: each reports as cancelled when the
		 * worker reaches it, which it does only with room for the result,
		 * so no unread result is ever dropped */
		for (size_t i = 0; i < w->queued; i++) {
			w->queue[i].cancelled = true;
		}
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeAllConditionVariable(&w->core.wake);
}

bool wf_copyworker_progress(WfCopyWorker *w, WfCopyProgress *out)
{
	bool busy;

	AcquireSRWLockShared(&w->core.lock);
	*out = w->progress;
	out->queued = w->queued;
	busy = w->running || w->queued;
	/* PAUSED is reported by checkpoint() once the worker is parked there.
	 * Reporting it as soon as a pause was requested said "nothing more is
	 * written" while the chunk in flight was still being written. */
	ReleaseSRWLockShared(&w->core.lock);
	return busy;
}

HANDLE wf_copyworker_event(const WfCopyWorker *w)
{
	return w->core.ready;
}

bool wf_copyworker_poll(WfCopyWorker *w, WfCopyResult *out)
{
	bool got = false;

	AcquireSRWLockExclusive(&w->core.lock);
	if (w->nresults) {
		*out = w->results[0];
		memmove(&w->results[0], &w->results[1],
		        (w->nresults - 1) * sizeof w->results[0]);
		w->nresults--;
		got = true;
	}
	if (!w->nresults) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}
