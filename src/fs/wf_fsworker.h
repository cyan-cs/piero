#ifndef WF_FSWORKER_H
#define WF_FSWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_fsport.h"
#include "wf_dir.h"

/*
 * Runs directory listings off the UI thread.
 *
 * Only the newest request matters: submitting while an older one is queued
 * replaces it, and while one is being enumerated the worker's synchronous
 * I/O is cancelled (CancelSynchronousIo, best effort: a hung SMB server may
 * not honour it). Consumers still reject stale generations: cancellation is
 * not proof that a result cannot arrive. Successful polls transfer ownership
 * of both snapshots; unpolled snapshots are freed by the worker.
 */
typedef struct WfFsWorker WfFsWorker;

typedef struct
{
	uint32_t gen;
	WfStatus st;
	WfDir *dir;    /* owned; NULL unless st == WF_OK */
	WfDir *parent; /* owned; NULL if absent or unreadable */
} WfFsResult;

WfStatus wf_fsworker_start(WfFsWorker **out);
/* Nonzero limit also bounds real-directory snapshots to 8 MiB, keeps only
 * one completed result and suppresses obsolete in-flight results. Zero keeps
 * the normal navigation limits and result ring. */
WfStatus wf_fsworker_start_limited(WfFsWorker **out, size_t limit);

WfWorkerStopReport wf_fsworker_stop(WfFsWorker *w);
WfWorkerStats wf_fsworker_stats(WfFsWorker *w);

/* Manual-reset event: signalled while results are waiting. */
HANDLE wf_fsworker_event(const WfFsWorker *w);

/* Pops one result; returns 0 when none are waiting. */
bool wf_fsworker_poll(WfFsWorker *w, WfFsResult *out);

/* The app-facing view of this worker. */
WfFsPort wf_fsworker_port(WfFsWorker *w);

/* Captured when the worker dequeues its next request; current work is unchanged.
 * The single producer sets this before submitting that request. */
void wf_fsworker_set_sort(WfFsWorker *w, WfSortKey key, bool descending);

/* Whether directories are listed before files (the default, true); read and
 * set the same way as the sort. */
void wf_fsworker_set_dirs_first(WfFsWorker *w, bool dirs_first);

#endif
