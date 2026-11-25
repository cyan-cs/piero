#include "../async/wf_workcore.h"
#include "wf_fsworker.h"

#include <stdlib.h>

#include "../archive/wf_archive.h"
#include "../path/wf_path.h"
#include "wf_enum_win.h"
#include "wf_pcplace_win.h"
#include "wf_sftpplace_win.h"

#define RESULT_CAP 8
#define STOP_WAIT_MS 1000

struct WfFsWorker
{
	WfWorkCore core;

	bool has_req;
	uint32_t req_gen;
	wchar_t *req_path, *req_parent;
	WfSortKey sort;
	bool descending;
	bool dirs_first; /* directories listed before files */
	size_t limit;    /* entries kept per listing; 0 = the usual maximum */

	WfFsResult q[RESULT_CAP]; /* ring; oldest dropped on overflow */
	size_t qhead, qn;
};

static void result_free(WfFsResult *r)
{
	wf_dir_free(r->dir);
	wf_dir_free(r->parent);
	r->dir = r->parent = NULL;
}

static void worker_free(void *owner)
{
	WfFsWorker *w = owner;

	for (size_t i = 0; i < w->qn; i++) {
		result_free(&w->q[(w->qhead + i) % RESULT_CAP]);
	}
	free(w->req_path);
	free(w->req_parent);
	free(w);
}

/* Caller holds the lock. */
static void push_result(WfFsWorker *w, WfFsResult r)
{
	size_t capacity = w->limit ? 1 : RESULT_CAP;
	if (capacity ==
	        w->qn) { /* consumer is far behind: only newest matter */
		result_free(&w->q[w->qhead]);
		w->qhead = (w->qhead + 1) % RESULT_CAP;
		w->qn--;
		if (UINT64_MAX != w->core.stats.discarded_results) {
			w->core.stats.discarded_results++;
		}
	}
	w->q[(w->qhead + w->qn++) % RESULT_CAP] = r;
	SetEvent(w->core.ready);
}

/* Synchronous cancellation is best-effort; enumeration also checks this
 * between batches so obsolete work does not require a cancellable IO call. */
static bool listing_cancelled(void *ctx)
{
	WfFsWorker *w = ctx;

	AcquireSRWLockShared(&w->core.lock);
	bool cancelled = w->core.stop || w->has_req;
	ReleaseSRWLockShared(&w->core.lock);
	return cancelled;
}

/* The order a listing was asked for. */
typedef struct
{
	WfSortKey key;
	bool descending, dirs_first;
} SortAsked;

/* A folder on disk, or a folder inside an archive. A folder on disk comes out
 * in the order asked for (`*ordered`): it is sorted once, as it is made. */
static WfStatus list_any(WfFsWorker *w,
        const wchar_t *path,
        const SortAsked *as,
        bool *ordered,
        WfDir **out)
{
	WfArchiveCtl ctl = {listing_cancelled, w};

	*ordered = false;
	if (wf_vpath_is(path)) {
		return wf_vpath_list(path, w->limit, &ctl, out);
	}
	if (wf_pcroot_is(path)) {
		return wf_pcplace_list(w->limit, out);
	}
	if (wf_sftp_place_is(path)) {
		return wf_sftp_place_list(path, w->limit ? w->limit : 100000,
		        listing_cancelled, w, out);
	}
	*ordered = true;
	return wf_enum_dir_win_ordered(path, w->limit,
	        w->limit ? (8u << 20) : 0, as->key, as->descending,
	        as->dirs_first, listing_cancelled, w, out);
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	/* the folder shown is in the order asked for; the folder on the left, only
	 * decoration, is by name */
	static const SortAsked BY_NAME = {WF_SORT_NAME, false, true};
	WfFsWorker *w = arg;

	for (;;) {
		WfFsResult r = {0, WF_OK, NULL, NULL};
		wchar_t *path, *parent;
		SortAsked asked;
		bool ordered, ordered_parent;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->has_req) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		r.gen = w->req_gen;
		asked.key = w->sort;
		asked.descending = w->descending;
		asked.dirs_first = w->dirs_first;
		path = w->req_path;
		parent = w->req_parent;
		w->req_path = w->req_parent = NULL;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		r.st = list_any(w, path, &asked, &ordered, &r.dir);
		if (WF_OK == r.st && r.dir && !ordered &&
		        (asked.key != WF_SORT_NAME || asked.descending ||
		                !asked.dirs_first)) {
			/* an archive or a remote place: ordered by name as it was made */
			r.st = wf_dir_sort_with(r.dir, asked.key,
			        asked.descending, asked.dirs_first);
			if (r.st != WF_OK) {
				wf_dir_free(r.dir);
				r.dir = NULL;
			}
		}
		if (WF_OK == r.st && parent &&
		        WF_OK !=
		                list_any(w, parent, &BY_NAME, &ordered_parent,
		                        &r.parent)) {
			r.parent = NULL; /* the left column is decoration */
		}
		free(path);
		free(parent);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		if (!w->core.stop && !(w->limit && w->has_req)) {
			push_result(w, r);
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

WfStatus wf_fsworker_start(WfFsWorker **out)
{
	return wf_fsworker_start_limited(out, 0);
}

WfStatus wf_fsworker_start_limited(WfFsWorker **out, size_t limit)
{
	WfFsWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	w->limit = limit;
	w->dirs_first = true;
	if (WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_fsworker_stop(WfFsWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

WfWorkerStats wf_fsworker_stats(WfFsWorker *w)
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

HANDLE wf_fsworker_event(const WfFsWorker *w)
{
	return w->core.ready;
}

bool wf_fsworker_poll(WfFsWorker *w, WfFsResult *out)
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

static WfStatus
submit(void *ctx, uint32_t gen, const wchar_t *path, const wchar_t *parent_path)
{
	WfFsWorker *w = ctx;
	wchar_t *p, *pp;

	if (!w || !path) {
		return WF_E_INVAL;
	}
	p = wf_path_dup(path);
	pp = parent_path ? wf_path_dup(parent_path) : NULL;
	if (!p || (parent_path && !pp)) {
		free(p);
		free(pp);
		return WF_E_NOMEM;
	}

	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(p);
		free(pp);
		return WF_E_INVAL;
	}
	if (w->has_req && UINT64_MAX != w->core.stats.superseded_requests) {
		w->core.stats.superseded_requests++;
	}
	free(w->req_path); /* a queued, not-yet-started request is superseded */
	free(w->req_parent);
	w->req_path = p;
	w->req_parent = pp;
	w->req_gen = gen;
	w->has_req = true;
	if (w->core.busy) {
		CancelSynchronousIo(
		        w->core.thread); /* harmless if it is between calls */
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

WfFsPort wf_fsworker_port(WfFsWorker *w)
{
	WfFsPort port = {w, submit};

	return port;
}

void wf_fsworker_set_sort(WfFsWorker *w, WfSortKey key, bool descending)
{
	AcquireSRWLockExclusive(&w->core.lock);
	w->sort = key;
	w->descending = descending;
	ReleaseSRWLockExclusive(&w->core.lock);
}

void wf_fsworker_set_dirs_first(WfFsWorker *w, bool dirs_first)
{
	AcquireSRWLockExclusive(&w->core.lock);
	w->dirs_first = dirs_first;
	ReleaseSRWLockExclusive(&w->core.lock);
}
