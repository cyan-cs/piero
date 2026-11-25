#ifndef WF_COPYWORKER_H
#define WF_COPYWORKER_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_copyengine_win.h"

/*
 * Runs copy jobs one after another on a thread of their own, so the screen
 * stays alive while files are read, written and read back. Jobs wait in a
 * short queue; the one running can be paused, resumed or cancelled. A cancel
 * makes the engine remove the temporary it was writing (.wfcopy-*.tmp in the
 * destination folder) on its own FileId, so only the file it made is removed,
 * never another with that name; a folder is built under a hidden .wfcopy-*.dir
 * name and taken apart the same way. Removal is attempted, not guaranteed: if
 * the system refuses (a program holds the file open, access denied) the
 * leftover stays hidden where it was made, a folder that cannot be taken apart
 * is reported as WF_CF_STAGING_LEFT, and a file stays in the copy journal (the
 * next start reports the unresolved record; nothing removes it by itself). A
 * temporary whose identity cannot be read right after it was made is never
 * removed: nothing is deleted except through the owned-delete proof, so it
 * stays under its announced name, with the copy reported as failed.
 * During normal service each accepted job yields one result. stop() cancels
 * the running job and discards queued jobs and pending results. It waits for
 * cleanup for a bounded time; unfinished cleanup retains its worker state.
 */
typedef struct WfCopyWorker WfCopyWorker;

#define WF_COPY_QUEUE_MAX 8

/* `journal_path` may be NULL (no crash journal: only for tests).
 * WF_OK means the receipt writer was opened before the thread was started.
 * Failure leaves *out NULL and never starts an unjournaled product worker. */
WfStatus wf_copyworker_start(WfCopyWorker **out, const wchar_t *journal_path);
WfWorkerStopReport wf_copyworker_stop(WfCopyWorker *w);

/* Copies `spec`. WF_E_RANGE when the queue is full. */
WfStatus wf_copyworker_submit(
        WfCopyWorker *w, const WfCopySpec *spec, uint32_t *job);
void wf_copyworker_set_paused(WfCopyWorker *w, bool paused);
bool wf_copyworker_paused(WfCopyWorker *w);
/* Cancels the running job; with `all`, also drops the queued ones (each of
 * those still yields a CANCELLED result). */
void wf_copyworker_cancel(WfCopyWorker *w, bool all);

/* Fills *out and returns true while a job is running or queued. */
bool wf_copyworker_progress(WfCopyWorker *w, WfCopyProgress *out);

HANDLE wf_copyworker_event(const WfCopyWorker *w);
/* One finished job's result; the caller frees it with wf_copyresult_free. */
bool wf_copyworker_poll(WfCopyWorker *w, WfCopyResult *out);

#endif
