#ifndef WF_CLIPWORKER_WIN_H
#define WF_CLIPWORKER_WIN_H

#include <windows.h>

#include "../async/wf_worker.h"
#include "wf_clipboard.h"

/*
 * Reads and publishes file lists on a worker thread, which is its
 * own single-threaded apartment (OleInitialize on that thread, nowhere else): the
 * program that owns the clipboard may answer slowly or never, and the UI must not
 * wait for it. One request is served at a time; a newer request replaces one
 * that has not started, and a result nobody has taken is replaced by a newer one.
 * Cancelling a request that is inside the system's call is not possible, so a
 * stuck call may remain until process exit: after a bounded wait, stop()
 * hands the thread over as a deferred completion (wf_worker.h).
 */
typedef struct WfClipWorker WfClipWorker;

WfStatus wf_clipworker_start(WfClipWorker **out);
WfWorkerStopReport wf_clipworker_stop(WfClipWorker *w);
/* True when nothing is in flight, nothing waits to be polled and the worker
 * holds nothing on the clipboard: it can be stopped without losing an answer or
 * the data it put there. Retaining a published data object keeps the worker
 * non-idle until shutdown, even if another program replaces the clipboard.
 * NULL: true. (DR-DRAFT-036) */
bool wf_clipworker_idle(WfClipWorker *w);

/* Manual-reset event: signalled while an answer waits. */
HANDLE wf_clipworker_event(const WfClipWorker *w);
/* Takes the answer; the caller owns `*out` (wf_clipfiles_free). */
bool wf_clipworker_poll(WfClipWorker *w, WfClipFiles *out);

WfClipFilesPort wf_clipworker_port(WfClipWorker *w);

#endif
