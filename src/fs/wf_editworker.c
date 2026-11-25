#include "../async/wf_workcore.h"
#include "wf_editworker.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_edit_win.h"
#include "wf_loc.h"
#include "wf_wslops_win.h"
#include "../base/wf_macro.h"

/* A rename is one handle-based call; this only bounds a hung network volume. */
#define STOP_WAIT_MS 5000

typedef struct
{
	bool present;
	uint32_t gen;
	WfEditKind kind;
	wchar_t *parent, *old_name, *new_name, *dest_parent;
	WfFileId parent_id, source_id, dest_parent_id;
} Job;

struct WfEditWorker
{
	WfWorkCore core;
	Job job;
	bool has_result;
	WfEditResult result;
};

static void job_free(Job *j)
{
	free(j->parent);
	free(j->old_name);
	free(j->new_name);
	free(j->dest_parent);
	memset(j, 0, sizeof *j);
}

static void worker_free(void *owner)
{
	WfEditWorker *w = owner;

	job_free(&w->job);
	free(w);
}

static void run_job(const Job *j, WfEditResult *r)
{
	WfEditReport report;
	WfStatus st;

	memset(r, 0, sizeof *r);
	memset(&report, 0, sizeof report);
	r->gen = j->gen;
	r->kind = j->kind;
	wcsncpy_s(
	        r->new_name, WF_ARRAY_LEN(r->new_name), j->new_name, _TRUNCATE);
	if (WF_EDIT_WSL_COPY == j->kind || WF_EDIT_WSL_DELETE == j->kind ||
	        WF_EDIT_WSL_REMOVE_EMPTY == j->kind ||
	        wf_wsl_unc_is(j->parent)) {
		/* a distribution has no file identity: its own operations,
		 * which never replace and never follow a link */
		WfWslReport w;

		memset(&w, 0, sizeof w);
		if (WF_EDIT_WSL_COPY == j->kind) {
			st = wf_wslops_copy_in(j->parent, j->old_name,
			        j->dest_parent, j->new_name, &w);
		} else if (WF_EDIT_WSL_DELETE == j->kind) {
			st = wf_wslops_delete_in(
			        j->parent, j->old_name, true, &w);
		} else if (WF_EDIT_WSL_REMOVE_EMPTY == j->kind) {
			st = wf_wslops_remove_if_empty(
			        j->parent, j->old_name, &w);
		} else if (WF_EDIT_MOVE == j->kind) {
			st = wf_wslops_rename(j->parent, j->old_name,
			        j->dest_parent, j->new_name, &w);
		} else if (WF_EDIT_RENAME == j->kind) {
			st = wf_wslops_rename(j->parent, j->old_name, j->parent,
			        j->new_name, &w);
		} else {
			st = wf_wslops_create_path(j->parent, j->new_name,
			        WF_EDIT_CREATE_DIR == j->kind, &w);
		}
		report.error = w.error;
		report.renamed = WF_OK == st;
		r->outcome.detail = w.reason ? w.reason : w.warning;
		r->outcome.partial = w.done;
	} else if (WF_EDIT_MOVE == j->kind) {
		st = wf_edit_move(j->parent, &j->parent_id, j->old_name,
		        &j->source_id, j->dest_parent, &j->dest_parent_id,
		        j->new_name, &report);
	} else if (WF_EDIT_RENAME == j->kind) {
		st = wf_edit_rename_checked(j->parent, &j->parent_id,
		        j->old_name, &j->source_id, j->new_name, &report);
	} else {
		st = wf_edit_create_path_checked(j->parent, &j->parent_id,
		        j->new_name, WF_EDIT_CREATE_DIR == j->kind, &report);
	}
	r->outcome.st = st;
	r->outcome.error = report.error;
	r->outcome.renamed = WF_OK == st || report.renamed;
	r->outcome.name_reason =
	        WF_NAME_OK != report.name ? wf_name_reason(report.name) : NULL;
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfEditWorker *w = arg;

	for (;;) {
		Job job;
		WfEditResult r;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->job.present) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			/* A request that never started is dropped: no
			 * filesystem call was made for it. */
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		job = w->job;
		memset(&w->job, 0, sizeof w->job);
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		run_job(&job, &r);
		job_free(&job);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		w->result = r;
		w->has_result = true;
		SetEvent(w->core.ready);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_editworker_start(WfEditWorker **out)
{
	WfEditWorker *w;

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

WfWorkerStopReport wf_editworker_stop(WfEditWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

WfWorkerStats wf_editworker_stats(WfEditWorker *w)
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

HANDLE wf_editworker_event(const WfEditWorker *w)
{
	return w->core.ready;
}

bool wf_editworker_poll(WfEditWorker *w, WfEditResult *out)
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

static wchar_t *dup_or_null(const wchar_t *s, bool *failed)
{
	wchar_t *copy;

	if (!s) {
		return NULL;
	}
	copy = wf_path_dup(s);
	if (!copy) {
		*failed = true;
	}
	return copy;
}

static WfStatus submit(void *ctx, uint32_t gen, const WfEditRequest *req)
{
	WfEditWorker *w = ctx;
	Job j;
	bool failed = false;

	if (!w || !req || !req->parent || !req->new_name ||
	        ((WF_EDIT_RENAME == req->kind || WF_EDIT_MOVE == req->kind ||
	                 WF_EDIT_WSL_COPY == req->kind ||
	                 WF_EDIT_WSL_DELETE == req->kind ||
	                 WF_EDIT_WSL_REMOVE_EMPTY == req->kind) &&
	                !req->old_name) ||
	        ((WF_EDIT_MOVE == req->kind || WF_EDIT_WSL_COPY == req->kind) &&
	                !req->dest_parent)) {
		return WF_E_INVAL;
	}
	memset(&j, 0, sizeof j);
	j.gen = gen;
	j.kind = req->kind;
	j.parent_id = req->parent_id;
	j.source_id = req->source_id;
	j.dest_parent_id = req->dest_parent_id;
	j.parent = dup_or_null(req->parent, &failed);
	j.old_name = dup_or_null(req->old_name, &failed);
	j.new_name = dup_or_null(req->new_name, &failed);
	j.dest_parent = dup_or_null(req->dest_parent, &failed);
	if (failed) {
		job_free(&j);
		return WF_E_NOMEM;
	}
	j.present = true;

	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || w->job.present || w->core.busy || w->has_result) {
		ReleaseSRWLockExclusive(&w->core.lock);
		job_free(&j);
		return WF_E_INVAL; /* one request at a time; never superseded */
	}
	w->job = j;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

WfEditPort wf_editworker_port(WfEditWorker *w)
{
	WfEditPort port = {w, submit};

	return port;
}
