#include "../async/wf_workcore.h"
#include "wf_launchworker.h"

#include <objbase.h>
#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"
#include "wf_launch_win.h"
#include "wf_terminal_win.h"
#include "../xfer/wf_xfer.h"
#include "../base/wf_macro.h"

/* A handler that is still starting after this long is left to finish. */
#define STOP_WAIT_MS 5000

typedef struct
{
	bool present;
	bool terminal; /* a terminal in `directory`; `path` and `id` are unused */
	uint32_t gen;
	wchar_t *path, *directory;
	WfFileId id;
} Job;

struct WfLaunchWorker
{
	WfWorkCore core;
	Job job;
	bool has_result;
	WfLaunchResult result;
};

static void job_free(Job *j)
{
	free(j->path);
	free(j->directory);
	memset(j, 0, sizeof *j);
}

static void worker_free(void *owner)
{
	WfLaunchWorker *w = owner;

	job_free(&w->job);
	free(w);
}

static void leaf_copy(const wchar_t *path, wchar_t *out, size_t cap)
{
	const wchar_t *leaf = path + wf_path_parent_len(path);

	while (L'\\' == *leaf || L'/' == *leaf) {
		leaf++;
	}
	wcsncpy_s(out, cap, leaf, _TRUNCATE);
}

static void run_job(const Job *j, WfLaunchResult *r)
{
	DWORD error = 0;

	memset(r, 0, sizeof *r);
	r->gen = j->gen;
	if (j->terminal) { /* directory path, truncated to the result's capacity */
		wcsncpy_s(r->name, WF_ARRAY_LEN(r->name), j->directory,
		        _TRUNCATE);
		r->outcome.st = wf_terminal_open_here(j->directory, &error);
		r->outcome.error = error;
		return;
	}
	leaf_copy(j->path, r->name, WF_ARRAY_LEN(r->name));
	r->outcome.st =
	        wf_launch_checked(j->path, &j->id, j->directory, &error);
	r->outcome.error = error;
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfLaunchWorker *w = arg;
	/* The shell wants an STA; COM failing only costs handlers that need it
	 * (they report their own error), so the worker still runs. */
	HRESULT com = CoInitializeEx(
	        NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

	for (;;) {
		Job job;
		WfLaunchResult r;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->job.present) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			/* Never started: the shell was not called for it. */
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
	if (SUCCEEDED(com)) {
		CoUninitialize();
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_launchworker_start(WfLaunchWorker **out)
{
	WfLaunchWorker *w;

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

WfWorkerStopReport wf_launchworker_stop(WfLaunchWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	return wf_wc_stop(&w->core, STOP_WAIT_MS, false, worker_free, w);
}

HANDLE wf_launchworker_event(const WfLaunchWorker *w)
{
	return w->core.ready;
}

bool wf_launchworker_poll(WfLaunchWorker *w, WfLaunchResult *out)
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

bool wf_launchworker_idle(WfLaunchWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->job.present && !w->has_result;
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

static WfStatus submit(void *ctx, uint32_t gen, const WfLaunchRequest *req)
{
	WfLaunchWorker *w = ctx;
	Job j;

	if (!w || !req || !req->directory ||
	        (!req->terminal &&
	                (!req->path || !wf_fileid_usable_to_read(&req->id)))) {
		return WF_E_INVAL;
	}
	memset(&j, 0, sizeof j);
	j.gen = gen;
	j.terminal = req->terminal;
	j.id = req->id;
	j.path = req->terminal ? NULL : wf_path_dup(req->path);
	j.directory = wf_path_dup(req->directory);
	if ((!req->terminal && !j.path) || !j.directory) {
		job_free(&j);
		return WF_E_NOMEM;
	}
	j.present = true;

	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || w->job.present || w->core.busy || w->has_result) {
		ReleaseSRWLockExclusive(&w->core.lock);
		job_free(&j);
		return WF_E_INVAL; /* one request at a time */
	}
	w->job = j;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

WfLaunchPort wf_launchworker_port(WfLaunchWorker *w)
{
	WfLaunchPort port = {w, submit};

	return port;
}
