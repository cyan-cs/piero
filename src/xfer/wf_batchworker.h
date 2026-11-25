#ifndef WF_BATCHWORKER_H
#define WF_BATCHWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_batchport.h"

/*
 * Runs a prepared batch rename (wf_batch_execute), or the recovery of old
 * journals (wf_batch_recover), on a thread of its own (DR-DRAFT-022, UI).
 *
 * Like the edit worker it carries receipts, so nothing is superseded or dropped:
 * one request at a time; run() and recover() refuse while one is queued, running
 * or not yet polled; every accepted request yields exactly one WfBxDone, unless
 * stop() discarded it before it began. A running batch is asked to stop with
 * cancel(): the engine looks at the flag before each step and rolls back what it
 * did. stop() sets the same flag and waits for a bounded time; unfinished
 * work retains its worker state until cleanup completes.
 *
 * `journal_dir` is where the .pbj files are (the state folder's batch\ folder);
 * it is created when the first journal is. Tests give a temporary folder.
 */
typedef struct WfBatchWorker WfBatchWorker;

WfStatus wf_batchworker_start(WfBatchWorker **out, const wchar_t *journal_dir);
/* Consumes the caller's reference. Requests cancellation and waits boundedly. */
WfWorkerStopReport wf_batchworker_stop(WfBatchWorker *w);

/* Manual-reset event: signalled while a result waits. */
HANDLE wf_batchworker_event(const WfBatchWorker *w);
bool wf_batchworker_poll(WfBatchWorker *w, WfBxDone *out);

WfBatchPort wf_batchworker_port(WfBatchWorker *w);

#endif
