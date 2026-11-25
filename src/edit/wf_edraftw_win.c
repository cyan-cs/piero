#include "../async/wf_workcore.h"
#include "wf_edraftw_win.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "wf_edraft_win.h"

#define DW_MAX 16 /* queued jobs, active jobs and unreported failures */

typedef struct
{
	wchar_t *path; /* owned */
	WfEncoding enc;
	char *text; /* owned */
	size_t n;
} Job;

struct WfDraftWriter
{
	WfWorkCore core;
	CONDITION_VARIABLE
	settled;       /* a write ended, or a waiting text was dropped */
	Job q[DW_MAX]; /* guarded by core.lock, with all that follows */
	size_t nq;
	const wchar_t *busy; /* the path being written now (the job's own) */
	wchar_t *failed
	        [DW_MAX]; /* paths whose last write failed, not yet told */
	size_t nfailed;
};

static bool same_path(const wchar_t *a, const wchar_t *b)
{
	/* the same spelling only: in a folder that tells case apart Foo.txt and
	 * foo.txt are two files, and a text must never be taken for the other's */
	return 0 == wcscmp(a, b);
}

static void job_free(Job *j)
{
	free(j->path);
	free(j->text);
	memset(j, 0, sizeof *j);
}

static void writer_free(void *owner)
{
	WfDraftWriter *w = owner;

	for (size_t i = 0; i < w->nq; i++) {
		job_free(&w->q[i]);
	}
	for (size_t i = 0; i < w->nfailed; i++) {
		free(w->failed[i]);
	}
	free(w);
}

/* guarded by the lock */
static void forget_failure(WfDraftWriter *w, const wchar_t *path)
{
	for (size_t i = 0; i < w->nfailed; i++) {
		if (same_path(w->failed[i], path)) {
			free(w->failed[i]);
			w->failed[i] = w->failed[--w->nfailed];
			return;
		}
	}
}

/* guarded by the lock */
static void note_failure(WfDraftWriter *w, wchar_t **path)
{
	for (size_t i = 0; i < w->nfailed; i++) {
		if (same_path(w->failed[i], *path)) {
			return;
		}
	}
	/* Admission reserves a failure slot for every queued or busy job.
	 * Transfer its path so reporting an error needs no further allocation. */
	w->failed[w->nfailed++] = *path;
	*path = NULL;
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfDraftWriter *w = arg;

	for (;;) {
		Job job;
		WfStatus st;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && 0 == w->nq) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (0 == w->nq) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break; /* stopped, and nothing is left to write */
		}
		job = w->q[0];
		memmove(&w->q[0], &w->q[1], (w->nq - 1) * sizeof w->q[0]);
		memset(&w->q[--w->nq], 0, sizeof w->q[0]);
		w->busy = job.path;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		st = wf_draft_write(job.path, job.enc, job.text, job.n);

		AcquireSRWLockExclusive(&w->core.lock);
		if (WF_OK == st) {
			forget_failure(w, job.path);
		} else {
			note_failure(w, &job.path);
		}
		w->busy = NULL;
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		ReleaseSRWLockExclusive(&w->core.lock);
		job_free(&job);
		WakeAllConditionVariable(&w->settled);
	}
	wf_wc_release(&w->core, writer_free, w);
	return 0;
}

WfStatus wf_draftw_start(WfDraftWriter **out)
{
	WfDraftWriter *w = calloc(1, sizeof *w);

	*out = NULL;
	if (!w) {
		return WF_E_NOMEM;
	}
	InitializeConditionVariable(&w->settled);
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfStatus wf_draftw_submit(WfDraftWriter *w,
        const wchar_t *path,
        WfEncoding enc,
        const char *utf8,
        size_t n)
{
	wchar_t *p;
	char *t;
	size_t i;

	if (!w || !path || !path[0] || (n && !utf8)) {
		return WF_E_INVAL;
	}
	p = _wcsdup(path);
	t = malloc(n ? n : 1);
	if (!p || !t) {
		free(p);
		free(t);
		return WF_E_NOMEM;
	}
	if (n) {
		memcpy(t, utf8, n);
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(p);
		free(t);
		return WF_E_INVAL;
	}
	for (i = 0; i < w->nq && !same_path(w->q[i].path, path); i++) {
	}
	if (i < w->nq) { /* a text of this file that waits: this one replaces it */
		free(w->q[i].text);
		free(p);
		w->q[i].text = t;
		w->q[i].n = n;
		w->q[i].enc = enc;
		if (UINT64_MAX != w->core.stats.superseded_requests) {
			w->core.stats.superseded_requests++;
		}
	} else if (w->nq + w->nfailed + (w->busy ? 1u : 0u) < DW_MAX) {
		w->q[w->nq++] = (Job){p, enc, t, n};
	} else {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(p);
		free(t);
		return WF_E_NOMEM;
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

bool wf_draftw_take_failure(WfDraftWriter *w, const wchar_t *path)
{
	bool failed = false;

	if (!w || !path) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	for (size_t i = 0; i < w->nfailed && !failed; i++) {
		failed = same_path(w->failed[i], path);
	}
	if (failed) {
		forget_failure(w, path);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return failed;
}

/* guarded by the lock */
static bool waiting_or_busy(
        const WfDraftWriter *w, const wchar_t *path, bool queued)
{
	if (w->busy && same_path(w->busy, path)) {
		return true;
	}
	for (size_t i = 0; queued && i < w->nq; i++) {
		if (same_path(w->q[i].path, path)) {
			return true;
		}
	}
	return false;
}

void wf_draftw_settle(WfDraftWriter *w, const wchar_t *path, bool drop)
{
	if (!w || !path) {
		return;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (drop) {
		for (size_t i = 0; i < w->nq; i++) {
			if (same_path(w->q[i].path, path)) {
				job_free(&w->q[i]);
				memmove(&w->q[i], &w->q[i + 1],
				        (w->nq - i - 1) * sizeof w->q[0]);
				memset(&w->q[--w->nq], 0, sizeof w->q[0]);
				break;
			}
		}
		forget_failure(w, path);
	}
	while (waiting_or_busy(w, path, !drop)) {
		/* the thread ending (it would never answer) ends the wait */
		if (!SleepConditionVariableSRW(
		            &w->settled, &w->core.lock, 1000, 0) &&
		        WAIT_OBJECT_0 ==
		                WaitForSingleObject(w->core.thread, 0)) {
			break;
		}
	}
	ReleaseSRWLockExclusive(&w->core.lock);
}

WfWorkerStopReport wf_draftw_stop(WfDraftWriter *w, DWORD wait_ms)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, wait_ms, false, writer_free, w);
}
