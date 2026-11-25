#include "../async/wf_workcore.h"
#include "wf_lockworker_win.h"
#include "wf_restart_manager_win.h"

#include <stdlib.h>
#include <string.h>

#include "../path/wf_path.h"

#define STOP_WAIT_MS 1500

struct WfLockWorker
{
	WfWorkCore core;
	WfProcEnv env;
	HANDLE stop_event; /* ends a wait for a closing program */

	bool has_req;
	WfLockReq req_kind;
	uint32_t req_gen;
	wchar_t *req_path;
	uint32_t req_pid;
	uint64_t req_start;
	WfLockAppType req_type;

	bool has_result;
	WfLockAnswer result; /* owned until polled */
};

static void worker_free(void *owner)
{
	WfLockWorker *w = owner;

	free(w->req_path);
	wf_lockanswer_free(&w->result);
	if (w->stop_event) {
		CloseHandle(w->stop_event);
	}
	free(w);
}

static void serve(WfLockWorker *w,
        WfLockReq kind,
        uint32_t gen,
        const wchar_t *path,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type,
        WfLockAnswer *a)
{
	memset(a, 0, sizeof *a);
	a->gen = gen;
	a->req = kind;
	a->pid = pid;
	a->start = start;
	if (WF_LOCKREQ_QUERY == kind) {
		a->st = wf_rm_query(path, &w->env, &a->list, &a->error);
	} else {
		WfProcAction r = WF_LOCKREQ_CLOSE == kind
		        ? wf_proc_request_close(pid, start, type,
		                  WF_LOCK_CLOSE_WAIT_MS, w->stop_event, &w->env)
		        : wf_proc_end(pid, start, type, WF_LOCK_END_WAIT_MS,
		                  &w->env);

		a->result = r.result;
		a->deny = r.deny;
		a->error = r.error;
	}
}

static DWORD WINAPI thread_main(LPVOID arg)
{
	WfLockWorker *w = arg;

	for (;;) {
		WfLockReq kind;
		uint32_t gen, pid;
		uint64_t start;
		WfLockAppType type;
		wchar_t *path;
		WfLockAnswer a;

		AcquireSRWLockExclusive(&w->core.lock);
		while (!w->core.stop && !w->has_req) {
			SleepConditionVariableSRW(
			        &w->core.wake, &w->core.lock, INFINITE, 0);
		}
		if (w->core.stop) {
			ReleaseSRWLockExclusive(&w->core.lock);
			break;
		}
		kind = w->req_kind;
		gen = w->req_gen;
		pid = w->req_pid;
		start = w->req_start;
		type = w->req_type;
		path = w->req_path;
		w->req_path = NULL;
		w->has_req = false;
		w->core.busy = true;
		ReleaseSRWLockExclusive(&w->core.lock);

		serve(w, kind, gen, path, pid, start, type, &a);
		free(path);

		AcquireSRWLockExclusive(&w->core.lock);
		w->core.busy = false;
		if (w->has_result) {
			wf_lockanswer_free(&w->result);
			if (UINT64_MAX != w->core.stats.discarded_results) {
				w->core.stats.discarded_results++;
			}
		}
		w->result = a;
		w->has_result = true;
		if (UINT64_MAX != w->core.stats.completed_requests) {
			w->core.stats.completed_requests++;
		}
		SetEvent(w->core.ready);
		ReleaseSRWLockExclusive(&w->core.lock);
	}
	wf_wc_release(&w->core, worker_free, w);
	return 0;
}

WfStatus wf_lockworker_start(WfLockWorker **out, const WfProcEnv *env)
{
	WfLockWorker *w;

	if (!out) {
		return WF_E_INVAL;
	}
	*out = NULL;
	w = calloc(1, sizeof *w);
	if (!w) {
		return WF_E_NOMEM;
	}
	if (env) {
		w->env = *env;
	}
	w->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!w->stop_event || WF_OK != wf_wc_start(&w->core, thread_main, w)) {
		if (w->stop_event) {
			CloseHandle(w->stop_event);
		}
		free(w);
		return WF_E_OS;
	}
	*out = w;
	return WF_OK;
}

WfWorkerStopReport wf_lockworker_stop(WfLockWorker *w)
{
	WfWorkerStopReport none = {0};

	if (!w) {
		return none;
	}
	SetEvent(w->stop_event);
	return wf_wc_stop(&w->core, STOP_WAIT_MS, true, worker_free, w);
}

HANDLE wf_lockworker_event(const WfLockWorker *w)
{
	return w->core.ready;
}

bool wf_lockworker_idle(WfLockWorker *w)
{
	bool idle;

	if (!w) {
		return true;
	}
	AcquireSRWLockShared(&w->core.lock);
	idle = !w->core.busy && !w->has_req && !w->has_result;
	ReleaseSRWLockShared(&w->core.lock);
	return idle;
}

bool wf_lockworker_poll(WfLockWorker *w, WfLockAnswer *out)
{
	bool got = false;

	if (!w || !out) {
		return false;
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->has_result) {
		*out = w->result;
		memset(&w->result, 0, sizeof w->result);
		w->has_result = false;
		got = true;
	}
	if (!w->has_result) {
		ResetEvent(w->core.ready);
	}
	ReleaseSRWLockExclusive(&w->core.lock);
	return got;
}

/* Queues one request unless one is already out. */
static WfStatus submit(WfLockWorker *w,
        WfLockReq kind,
        uint32_t gen,
        const wchar_t *path,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type)
{
	wchar_t *copy = NULL;

	if (!w || !gen) {
		return WF_E_INVAL;
	}
	if (WF_LOCKREQ_QUERY == kind) {
		if (!path) {
			return WF_E_INVAL;
		}
		copy = wf_path_dup(path);
		if (!copy) {
			return WF_E_NOMEM;
		}
	} else if (!pid || !start) {
		return WF_E_INVAL; /* a process is named by pid and start time */
	}
	AcquireSRWLockExclusive(&w->core.lock);
	if (w->core.stop || w->has_req || w->core.busy || w->has_result) {
		ReleaseSRWLockExclusive(&w->core.lock);
		free(copy);
		return WF_E_INVAL;
	}
	w->has_req = true;
	w->req_kind = kind;
	w->req_gen = gen;
	w->req_path = copy;
	w->req_pid = pid;
	w->req_start = start;
	w->req_type = type;
	ReleaseSRWLockExclusive(&w->core.lock);
	WakeConditionVariable(&w->core.wake);
	return WF_OK;
}

static WfStatus port_query(void *ctx, uint32_t gen, const wchar_t *path)
{
	return submit(
	        ctx, WF_LOCKREQ_QUERY, gen, path, 0, 0, WF_LOCKAPP_UNKNOWN);
}

static WfStatus port_close(void *ctx,
        uint32_t gen,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type)
{
	return submit(ctx, WF_LOCKREQ_CLOSE, gen, NULL, pid, start, type);
}

static WfStatus port_end(void *ctx,
        uint32_t gen,
        uint32_t pid,
        uint64_t start,
        WfLockAppType type)
{
	return submit(ctx, WF_LOCKREQ_END, gen, NULL, pid, start, type);
}

WfLockPort wf_lockworker_port(WfLockWorker *w)
{
	WfLockPort p = {w, port_query, port_close, port_end};

	return p;
}
